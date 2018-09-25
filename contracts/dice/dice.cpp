/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include <utility>
#include <vector>
#include <string>
#include <eosiolib/eosio.hpp>
#include <eosiolib/time.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/contract.hpp>
#include <eosiolib/crypto.h>

using eosio::key256;
using eosio::indexed_by;
using eosio::const_mem_fun;
using eosio::asset;
using eosio::permission_level;
using eosio::action;
using eosio::print;
using eosio::name;

class dice : public eosio::contract {
   public:
      const uint32_t FIVE_MINUTES = 5*60;

      dice(account_name self)
      :eosio::contract(self),
       offers(_self, _self),
       games(_self, _self),
       global_dices(_self, _self),
       accounts(_self, _self)
      {}

      // 玩家投注，附带 sha256(secret)，类似流水号，唯一标识单子
      void offerbet(const asset& bet, const account_name player, const checksum256& commitment) {
         // 检查投注金额的合法性
         eosio_assert( bet.symbol == CORE_SYMBOL, "only core token allowed" );
         eosio_assert( bet.is_valid(), "invalid bet" );
         eosio_assert( bet.amount > 0, "must bet positive quantity" );

         eosio_assert( !has_offer( commitment ), "offer with this commitment already exist" );

         // 检查发起者权限
         require_auth( player );

         // 检查游戏账户是否存在，游戏账户是在充值时创建的
         auto cur_player_itr = accounts.find( player );
         eosio_assert(cur_player_itr != accounts.end(), "unknown account");

         // 保存新的单子
         auto new_offer_itr = offers.emplace(_self, [&](auto& offer){
            offer.id         = offers.available_primary_key();
            offer.bet        = bet;
            offer.owner      = player;
            offer.commitment = commitment;
            offer.gameid     = 0;
         });

         // 查找下注数量一致的其他offer
         auto idx = offers.template get_index<N(bet)>();
         auto matched_offer_itr = idx.lower_bound( (uint64_t)new_offer_itr->bet.amount );

         if( matched_offer_itr == idx.end()
            || matched_offer_itr->bet != new_offer_itr->bet
            || matched_offer_itr->owner == new_offer_itr->owner ) {

            // 没有找到匹配的offer, 修改玩家信息(挂单)
            accounts.modify( cur_player_itr, 0, [&](auto& acnt) {
               eosio_assert( acnt.eos_balance >= bet, "insufficient balance" );
               acnt.eos_balance -= bet;
               acnt.open_offers++;
            });

         } else {
            // 找到了对应的单子
            auto gdice_itr = global_dices.begin(); // 取第一条记录
            if( gdice_itr == global_dices.end() ) {
               // 如果是第一笔成交
               gdice_itr = global_dices.emplace(_self, [&](auto& gdice){
                  gdice.nextgameid=0;
               });
            }

            // Increment global game counter
            global_dices.modify(gdice_itr, 0, [&](auto& gdice){
               gdice.nextgameid++;
            });

            // 创建新的一局游戏, 包含玩家1以及玩家2的投注信息
            auto game_itr = games.emplace(_self, [&](auto& new_game){
               new_game.id       = gdice_itr->nextgameid;
               new_game.bet      = new_offer_itr->bet;
               new_game.deadline = eosio::time_point_sec(0);

               new_game.player1.commitment = matched_offer_itr->commitment;
               memset(&new_game.player1.reveal, 0, sizeof(checksum256));

               new_game.player2.commitment = new_offer_itr->commitment;
               memset(&new_game.player2.reveal, 0, sizeof(checksum256));
            });

            // 更新两个单子数据
            idx.modify(matched_offer_itr, 0, [&](auto& offer){
               offer.bet.amount = 0;
               offer.gameid = game_itr->id;
            });

            offers.modify(new_offer_itr, 0, [&](auto& offer){
               offer.bet.amount = 0;
               offer.gameid = game_itr->id;
            });

            // 更新两个玩家的账户信息
            accounts.modify( accounts.find( matched_offer_itr->owner ), 0, [&](auto& acnt) {
               acnt.open_offers--;
               acnt.open_games++;
            });

            accounts.modify( cur_player_itr, 0, [&](auto& acnt) {
               eosio_assert( acnt.eos_balance >= bet, "insufficient balance" );
               acnt.eos_balance -= bet;
               acnt.open_games++;
            });
         }
      }

      // 取消单子
      void canceloffer( const checksum256& commitment ) {
         // 根据流水号找到单子
         auto idx = offers.template get_index<N(commitment)>();
         auto offer_itr = idx.find( offer::get_commitment(commitment) );

         eosio_assert( offer_itr != idx.end(), "offer does not exists" );
         eosio_assert( offer_itr->gameid == 0, "unable to cancel offer" );
         // 检查权限
         require_auth( offer_itr->owner );

         // 更新账户数据
         auto acnt_itr = accounts.find(offer_itr->owner);
         accounts.modify(acnt_itr, 0, [&](auto& acnt){
            acnt.open_offers--;
            acnt.eos_balance += offer_itr->bet;
         });

         // 删除单子
         idx.erase(offer_itr);
      }

      // 两个玩家各自开奖。commitment: sha256(secret), source: secret
      void reveal( const checksum256& commitment, const checksum256& source ) {
         // 校验签名
         assert_sha256( (char *)&source, sizeof(source), (const checksum256 *)&commitment );

         // 根据流水号找到订单
         auto idx = offers.template get_index<N(commitment)>();
         auto curr_revealer_offer = idx.find( offer::get_commitment(commitment)  );

         eosio_assert(curr_revealer_offer != idx.end(), "offer not found");
         eosio_assert(curr_revealer_offer->gameid > 0, "unable to reveal");

         // 根据订单中保存的游戏id(唯一标识每局游戏)找到这局游戏
         auto game_itr = games.find( curr_revealer_offer->gameid );

         player curr_reveal = game_itr->player1;
         player prev_reveal = game_itr->player2;

         if( !is_equal(curr_reveal.commitment, commitment) ) {
            std::swap(curr_reveal, prev_reveal);
         }

         eosio_assert( is_zero(curr_reveal.reveal) == true, "player already revealed");

         if( !is_zero(prev_reveal.reveal) ) {
            // 如果另一个玩家已经开奖, 则表示双方都同意开奖了，即刻开奖
            checksum256 result;
            // 对两个玩家的两个字段一起进行sha256运算
            sha256( (char *)&game_itr->player1, sizeof(player)*2, &result);

            auto prev_revealer_offer = idx.find( offer::get_commitment(prev_reveal.commitment) );

            int winner = result.hash[1] < result.hash[0] ? 0 : 1;

            // 清算
            if( winner ) {
               pay_and_clean(*game_itr, *curr_revealer_offer, *prev_revealer_offer);
            } else {
               pay_and_clean(*game_itr, *prev_revealer_offer, *curr_revealer_offer);
            }

         } else {
            // 如果另一个玩家还没有开奖，则把自己的secret保存起来，表示我同意开奖
            games.modify(game_itr, 0, [&](auto& game){

               if( is_equal(curr_reveal.commitment, game.player1.commitment) )
                  game.player1.reveal = source;
               else
                  game.player2.reveal = source;
               // 给5分钟过期时间。意思是A同意开奖，B在5分钟内还没开奖，这局游戏就算A赢了
               game.deadline = eosio::time_point_sec(now() + FIVE_MINUTES);
            });
         }
      }

      // 清算过期的游戏。A同意开奖，B在5分钟内还没开奖，这局游戏就算A赢了
      void claimexpired( const uint64_t gameid ) {

         auto game_itr = games.find(gameid);

         eosio_assert(game_itr != games.end(), "game not found");
         eosio_assert(game_itr->deadline != eosio::time_point_sec(0) && eosio::time_point_sec(now()) > game_itr->deadline, "game not expired");

         auto idx = offers.template get_index<N(commitment)>();
         auto player1_offer = idx.find( offer::get_commitment(game_itr->player1.commitment) );
         auto player2_offer = idx.find( offer::get_commitment(game_itr->player2.commitment) );

         if( !is_zero(game_itr->player1.reveal) ) {
            eosio_assert( is_zero(game_itr->player2.reveal), "game error");
            // player1已同意，player2没有同意
            pay_and_clean(*game_itr, *player1_offer, *player2_offer);
         } else {
            eosio_assert( is_zero(game_itr->player1.reveal), "game error");
            pay_and_clean(*game_itr, *player2_offer, *player1_offer);
         }

      }

      // 充值资产。投注前必须先充值
      void deposit( const account_name from, const asset& quantity ) {
         
         eosio_assert( quantity.is_valid(), "invalid quantity" );
         eosio_assert( quantity.amount > 0, "must deposit positive quantity" );

         auto itr = accounts.find(from);
         if( itr == accounts.end() ) {
            // 玩家不存在就创建玩家
            itr = accounts.emplace(_self, [&](auto& acnt){
               acnt.owner = from;
            });
         }

         // 使用玩家赋予的code权限转走玩家eos给合约账户
         action(
            permission_level{ from, N(active) },
            N(eosio.token), N(transfer),
            std::make_tuple(from, _self, quantity, std::string(""))
         ).send();

         // 给玩家加上余额
         accounts.modify( itr, 0, [&]( auto& acnt ) {
            acnt.eos_balance += quantity;
         });
      }

      // 提现
      void withdraw( const account_name to, const asset& quantity ) {
         require_auth( to );

         eosio_assert( quantity.is_valid(), "invalid quantity" );
         eosio_assert( quantity.amount > 0, "must withdraw positive quantity" );

         auto itr = accounts.find( to );
         eosio_assert(itr != accounts.end(), "unknown account");

         accounts.modify( itr, 0, [&]( auto& acnt ) {
            eosio_assert( acnt.eos_balance >= quantity, "insufficient balance" );
            acnt.eos_balance -= quantity;
         });

         action(
            permission_level{ _self, N(active) },
            N(eosio.token), N(transfer),
            std::make_tuple(_self, to, quantity, std::string(""))
         ).send();

         // 如果玩家余额是0也没有参与游戏，则删掉玩家数据
         if( itr->is_empty() ) {
            accounts.erase(itr);
         }
      }

   private:
      //@abi table offer i64
      struct offer {
         uint64_t          id;
         account_name      owner;
         asset             bet;
         checksum256       commitment;
         uint64_t          gameid = 0;

         uint64_t primary_key()const { return id; }

         uint64_t by_bet()const { return (uint64_t)bet.amount; }

         key256 by_commitment()const { return get_commitment(commitment); }

         static key256 get_commitment(const checksum256& commitment) {
            const uint64_t *p64 = reinterpret_cast<const uint64_t *>(&commitment);
            return key256::make_from_word_sequence<uint64_t>(p64[0], p64[1], p64[2], p64[3]);
         }

         EOSLIB_SERIALIZE( offer, (id)(owner)(bet)(commitment)(gameid) )
      };

      typedef eosio::multi_index< N(offer), offer,
         indexed_by< N(bet), const_mem_fun<offer, uint64_t, &offer::by_bet > >,
         indexed_by< N(commitment), const_mem_fun<offer, key256,  &offer::by_commitment> >
      > offer_index;

      struct player {
         checksum256 commitment;
         checksum256 reveal;

         EOSLIB_SERIALIZE( player, (commitment)(reveal) )
      };

      //@abi table game i64
      struct game {
         uint64_t id;
         asset    bet;
         eosio::time_point_sec deadline;
         player   player1;
         player   player2;

         uint64_t primary_key()const { return id; }

         EOSLIB_SERIALIZE( game, (id)(bet)(deadline)(player1)(player2) )
      };

      typedef eosio::multi_index< N(game), game> game_index;

      //@abi table global i64
      struct global_dice {
         uint64_t id = 0;
         uint64_t nextgameid = 0;

         uint64_t primary_key()const { return id; }

         EOSLIB_SERIALIZE( global_dice, (id)(nextgameid) )
      };

      typedef eosio::multi_index< N(global), global_dice> global_dice_index;

      //@abi table account i64
      struct account {
         account( account_name o = account_name() ):owner(o){}

         account_name owner;
         asset        eos_balance;
         uint32_t     open_offers = 0;
         uint32_t     open_games = 0;

         bool is_empty()const { return !( eos_balance.amount | open_offers | open_games ); }

         uint64_t primary_key()const { return owner; }

         EOSLIB_SERIALIZE( account, (owner)(eos_balance)(open_offers)(open_games) )
      };

      typedef eosio::multi_index< N(account), account> account_index;

      offer_index       offers;
      game_index        games;
      global_dice_index global_dices;
      account_index     accounts;

      // 检查 sha256(secret) 是否提供过
      bool has_offer( const checksum256& commitment )const {
         auto idx = offers.template get_index<N(commitment)>();
         auto itr = idx.find( offer::get_commitment(commitment) );
         return itr != idx.end();
      }

      bool is_equal(const checksum256& a, const checksum256& b)const {
         return memcmp((void *)&a, (const void *)&b, sizeof(checksum256)) == 0;
      }

      bool is_zero(const checksum256& a)const {
         const uint64_t *p64 = reinterpret_cast<const uint64_t*>(&a);
         return p64[0] == 0 && p64[1] == 0 && p64[2] == 0 && p64[3] == 0;
      }

      void pay_and_clean(const game& g, const offer& winner_offer,
          const offer& loser_offer) {

         // 更新赢家的信息
         auto winner_account = accounts.find(winner_offer.owner);
         accounts.modify( winner_account, 0, [&]( auto& acnt ) {
            acnt.eos_balance += 2*g.bet;
            acnt.open_games--;
         });

         // 更新输家的信息
         auto loser_account = accounts.find(loser_offer.owner);
         accounts.modify( loser_account, 0, [&]( auto& acnt ) {
            acnt.open_games--;
         });

         if( loser_account->is_empty() ) {
            accounts.erase(loser_account);
         }
         // 删除这局游戏数据, 包括成交信息以及赢家输家单子
         games.erase(g);
         offers.erase(winner_offer);
         offers.erase(loser_offer);
      }
};

EOSIO_ABI( dice, (offerbet)(canceloffer)(reveal)(claimexpired)(deposit)(withdraw) )
