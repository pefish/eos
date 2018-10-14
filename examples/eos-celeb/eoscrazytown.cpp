#include "eoscrazytown.hpp"

// @abi action
void eoscrazytown::init(const checksum256 &hash)
{
    require_auth(_self);

    // clear() ;
    // _global.remove();
    /*
    auto g = _global.get_or_create( _self, st_global{.hash = hash});
    g.hash = hash;
    _global.set(g, _self); */

    for (int i=0;i<100;++i) {
        auto itr = bags.find(i);
        bags.modify(itr, 0, [&](auto &t) {
            t.price = 1000;
        });
    }
/*
    _bagsglobal.remove();

    auto g = _bagsglobal.get_or_create(_self, bagsglobal{
         .pool = 0, .team = 0, .last = N(vitalik11111), .st = 1539403200, .ed = 1539403200 + 60 * 60 * 24});


    _bagsglobal.set(g, _self);*/
}
// @abi action
void eoscrazytown::clear()
{
    require_auth(_self);

    //_global.remove();

    _bagsglobal.remove();

    // multi_index can't erase when the format changed

    /*
        auto it = db_lowerbound_i64(_self, _self, N(global), 0);
        while (it >= 0) {
            auto del = it;
            uint64_t dummy;
            it = db_next_i64(it, &dummy);
            db_remove_i64(del);
}*/

    //while (players.begin() != players.end()) {
    //    players.erase(players.begin());
    // }
}

void eoscrazytown::newbag(account_name &from, asset &eos)
{

    require_auth(_self);
    for (int i=0;i<10;++i){
        bags.emplace(from, [&](auto &p) {
            p.id = bags.available_primary_key();
            p.owner = from;
            p.price = eos.amount;
        });
    }
}

void eoscrazytown::setslogan(account_name &from, uint64_t id, string memo)
{
    auto itr = bags.find(id);
    eosio_assert(itr != bags.end(), "no character exist");
    eosio_assert(memo.size() <= 64, "too long");
    eosio_assert(from == itr->owner, "not the owner...");
    bags.modify(itr, from, [&](auto &t) {
        t.slogan = memo;
    });
}

// input
/*
void eoscrazytown::onTransfer(account_name &from, account_name &to, asset &eos, string &memo) {
    if (to != _self) return ;

    require_auth(from);
    eosio_assert(eos.is_valid(), "Invalid token transfer...");
    eosio_assert(eos.symbol == EOS_SYMBOL, "only EOS token is allowed");
    eosio_assert(eos.amount > 0, "must bet a positive amount");
    eosio_assert(memo != "" , "must have bets in memo") ;
    eosio_assert(memo.size() >= 21  , "bets wrong...") ;


    // todo: input check non-num
    vector<int64_t> vbets ;
    int64_t totalBets = 0 ;
    eosio_assert( eoscrazytown::checkBets( eos, memo, vbets, totalBets ), "Bets not equal to amount.");
    eosio_assert( totalBets >= 1, "Bets should >= 1=");
    eosio_assert( totalBets <= 100, "Bets should not > 100");

    const auto& sym = eosio::symbol_type(EOS_SYMBOL).name();
    accounts eos_account(N(eosio.token), _self);
    auto old_balance = eos_account.get(sym).balance;

    eosio_assert( totalBets <= old_balance.amount / 2, "Bets too big");
    auto itr = players.find(from);
    if (itr == players.end()) {
        players.emplace(_self, [&](auto& p) {
            p.account = from;
            p.vbets = vbets ;
        });
    } else {
        eosio_assert( false, "Already bet.");
        return ;
    }
}*/

// input
void eoscrazytown::onTransfer(account_name &from, account_name &to, asset &eos, string &memo)
{
    if (to != _self)
        return;

    require_auth(from);
    eosio_assert(eos.is_valid(), "Invalid token transfer...");
    eosio_assert(eos.symbol == EOS_SYMBOL, "only EOS token is allowed");
    eosio_assert(eos.amount > 0, "must buy a positive amount");
    //    eosio_assert(memo != "" , "must have something in memo") ;
    //    eosio_assert(memo.size() >= 21  , "bets wrong...") ;

    if (memo.substr(0, 3) == "buy")
    {

        auto g = _bagsglobal.get();

        eosio_assert( g.st <= now() && now() <= g.ed, "not correct time.");


        memo.erase(0, 4); // 删掉 'buy '
        std::size_t s = memo.find(' ');
        if (s == string::npos)
        {
            s = memo.size();
        }

        auto id = string_to_price(memo.substr(0, s)); // 名人id
        eosio_assert(id <= 100 || now() >= 1539403200 + 8*60*60, "no character exist");
        //  auto id = 0;
        memo.erase(0, s + 1); // 删掉名人id以及一个空格，剩下的是邀请者
        auto itr = bags.find(id);
        eosio_assert(itr != bags.end(), "no character exist");
        eosio_assert(eos.amount >= itr->next_price(), "no enough eos");
        eosio_assert(from != itr->owner, "cannot buy with yourself" );

        asset d(eos.amount - itr->next_price(), EOS_SYMBOL);

        if (d.amount > 0 && _self != from){

            // 多余的钱给退回去
            action( // winner winner chicken dinner
                    permission_level{_self, N(active)},
                    TOKEN_CONTRACT, N(transfer),
                    make_tuple(_self, from, d,
                               std::string("refund")))
                    .send();
        }

        d.amount = itr->next_price() - itr->price;

        auto ref_b = d;
        ref_b.amount /= 10; // 拿出溢出部分的10%给邀请人


        auto ref = eosio::string_to_name(memo.c_str());
        if (is_account(ref) && ref != from && _self != from)
        {
            // 给邀请人发放奖励
            if (ref_b.amount > 0) {
                action( // winner winner chicken dinner
                        permission_level{_self, N(active)},
                        N(eosio.token), N(transfer),
                        make_tuple(_self, ref, ref_b,
                                   std::string("ref bonus")))
                        .send();
            }
        }
        else
        {
            g.team += ref_b.amount; // 没有邀请人就放入奖池
        }
        d.amount -= ref_b.amount * 4;

        g.team += ref_b.amount * 2; // 20%给奖池
        g.pool += ref_b.amount; // 60%放入已发放的奖励中
        g.last = from;
        g.ed = now() + 60 * 60 * 24;

        _bagsglobal.set(g, _self);

        auto delta = d;
        delta.amount += itr->price; // 准备60%以及上一次购买的成本

        if(delta.amount > 0 &&  _self !=itr->owner){
            // 成本以及60%利润给上一个购买人
            action( // winner winner chicken dinner
                    permission_level{_self, N(active)},
                    N(eosio.token), N(transfer),
                    make_tuple(_self, itr->owner, delta,
                               std::string("next hodl")))
                    .send();
        }

        bags.modify(itr, 0, [&](auto &t) {
            t.owner = from;
            t.price = itr->next_price();
        });

        return;
    }

    // todo: input check non-num
    vector<int64_t> vbets;
    int64_t totalBets = 0;
    eosio_assert(eoscrazytown::checkBets(eos, memo, vbets, totalBets), "Bets not equal to amount.");
    auto itr = players.find(from);
    if (itr == players.end())
    {
        players.emplace(_self, [&](auto &p) {
            p.account = from;
            p.vbets = vbets;
        });
    }
    else
    {
        eosio_assert(false, "Already bet.");
        return;
    }
}

// Output
void eoscrazytown::reveal(const checksum256 &seed, const checksum256 &hash)
{ // hash for next round
    require_auth(_self);

    //   eosio_assert(players.begin() != players.end(), "must have at least one player");

    card dragon = seed.hash[0] % 52;
    card tiger = seed.hash[1] % 52;
    const rec_reveal _reveal{
            .dragon = dragon,
            .tiger = tiger,
            .server_hash = _global.get().hash,
            .client_seed = seed,
    };
    /*
    // singleton -> _global
    auto g = _global.get();
    g.dragon = dragon ;
    g.tiger = tiger ;
    _global.set(g, _self);
    */
    action( // give result to client
            permission_level{_self, N(active)},
            _self, N(receipt), _reveal)
            .send();

    auto result = getResult(_reveal.dragon, _reveal.tiger);

    string beton;
    string presult = "";
    int64_t bonus;
    const char o = 'O';
    for (const auto &p : players)
    {
        auto &bets = p.vbets;
        beton = getBeton(bets);

        bonus = 0;
        presult = "";
        // exp:
        // r 2.0: O X X X X X X X X X X // no space !
        // beton: O X X O X X O X O O O // no space !
        if (result[2] == o)
        { // (3) draw
            if (result[2] == beton[2])
                bonus += bets[2] + bets[2] * DRAW; // (3)
            bonus += bets[0] / 2;
            bonus += bets[1] / 2;
            /*
            if ( result[3] == beton[3] ) bonus += bets[3] + bets[3] * COLOR ; // (4)
            if ( result[4] == beton[4] ) bonus += bets[4] + bets[4] * COLOR ; // (5)

            if ( result[5] == beton[5] ) bonus += bets[5] + bets[5] * COLOR ; // (6)
            if ( result[6] == beton[6] ) bonus += bets[6] + bets[6] * COLOR ; // (7)
            */
        }
        else
        {
            if (result[0] == o)
            {
                if (result[0] == beton[0])
                {
                    bonus += bets[0] + bets[0] * SIDE; // (1)
                    presult += '1';
                }
            }
            else
            { // result[1] == o
                if (result[1] == beton[1])
                {
                    bonus += bets[1] + bets[1] * SIDE; // (2)
                    presult += '2';
                }
            }

            if (result[3] == beton[3])
            {
                bonus += bets[3] + bets[3] * COLOR; // (4)
                presult += '4';
            }

            if (result[4] == beton[4])
            {
                bonus += bets[4] + bets[4] * COLOR; // (5)
                presult += '5';
            }

            if (result[5] == beton[5])
            {
                bonus += bets[5] + bets[5] * COLOR; // (6)
                presult += '6';
            }
            if (result[6] == beton[6])
            {
                bonus += bets[6] + bets[6] * COLOR; // (7)
                presult += '7';
            }

            if (result[7] == beton[7])
            {
                bonus += bets[7] + bets[7] * ODD; // (8)
                presult += '8';
            }
            if (result[8] == beton[8])
            {
                bonus += bets[8] + bets[8] * EVEN; // (9)
                presult += '9';
            }
            if (result[9] == beton[9])
            {
                bonus += bets[9] + bets[9] * ODD; // (10)
                presult += 'A';
            }
            if (result[10] == beton[10])
            {
                bonus += bets[10] + bets[10] * EVEN; // (11)
                presult += 'B';
            }
        }

        send_defer_action(
                permission_level{_self, N(active)},
                TOKEN_CONTRACT, N(transfer),
                make_tuple(_self, p.account, asset(bonus, EOS_SYMBOL),
                           bonus != 0 ? string("Winner Winner Chicken Dinner. " + presult) : string("Better Luck Next Time")));
    }

    auto g = _global.get();
    g.hash = hash;
    g.dragon = dragon;
    g.tiger = tiger;
    _global.set(g, _self);

    while (players.begin() != players.end())
    {
        players.erase(players.begin());
    }

    // init( hash ) ;
}