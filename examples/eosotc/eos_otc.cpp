/**
 *  @dev minakokojima
 */

#include "eosotcbackup.hpp"


void eosotcbackup::init() {
    require_auth(_self);
}

// 清除所有订单
void eosotcbackup::clean() {
    require_auth(_self);
    while(offers.begin() != offers.end()) {
        offers.erase(offers.begin());
    }
}

uint64_t string_to_price(string s) {
    uint64_t z = 0;
    for (int i=0;i<s.size();++i) {
        if ('0' <= s[i] && s[i] <= '9') {
            z *= 10;
            z += s[i] - '0';
        }
    }
    return z;
}

void eosotcbackup::ask(account_name owner, extended_asset bid, extended_asset ask) {
    eosio_assert(bid.contract != N("eosotcbackup"), "fake" );
    eosio_assert(ask.contract != N("eosotcbackup"), "fake" );
    order_index orders(_self, ask.contract); // 要买的币都有一张order表

    auto id = orders.available_primary_key();

    orders.emplace(_self, [&](auto& o) {
        o.id = id;
        o.owner = owner;
        o.bid = bid; // 冲的是哪个币
        o.ask = ask; // 要买的是哪个币
        action(permission_level{_self, N(active)},
               _self, N(receipt), o
        ).send();
    });
}

void eosotcbackup::take(account_name owner, uint64_t order_id, extended_asset bid, extended_asset ask) {

    eosio_assert(bid.contract != N("eosotcbackup"), "fake currency" );
    eosio_assert(ask.contract != N("eosotcbackup"), "fake currency" );

    order_index orders(_self, bid.contract);
    auto itr = orders.find(order_id);
    if (itr->bid.contract == N("eosotcbackup") ){
        eosio_assert(false, "fake currency");
    }

    eosio_assert(itr != orders.end(), "order is not exist.");

    // 下单者 1EOS要买2CNY，吃单者 4CNY要买3个EOS，1 * 4 < 2 * 3 的话，不能成交
    eosio_assert(
            uint128_t(itr->bid.amount) * bid.amount > uint128_t(itr->ask.amount) * ask.amount,
            "price is too high");

    auto t = now();

    if (itr->bid.amount <= ask.amount) { // 下单者只是1个EOS去买，但吃单者需要买3个EOS
        // 下单者 1EOS要买2CNY，吃单者 4CNY要买2个EOS
        asset _bid = itr->bid;
        asset _ask = itr->ask;

        action(permission_level{_self, N(active)},
               _self, N(receipt), *itr)
                .send();

        // 将 bid 1EOS 交给吃单者
        action(
                permission_level{_self, N(active)},
                ask.contract, N(transfer),
                make_tuple(_self, owner, _bid,
                           std::string("trade success"))
        ).send();

        // 将 ask 2CNY 交给下单者
        action(
                permission_level{_self, N(active)},
                bid.contract, N(transfer),
                make_tuple(_self, itr->owner, _ask,
                           std::string("trade success"))
        ).send();

        orders.erase(itr);  // 多余的2CNY被送给合约了, 不合理？？
    } else {  // 下单者是3个EOS去买，但吃单者只需要买1个EOS
        // 下单者 3EOS要买6CNY，吃单者 2CNY要买1个EOS
        asset _bid = ask; // 1EOS
        asset _ask = bid; // 2CNY

                // ???
        action(permission_level{_self, N(active)},
               _self, N(receipt), *itr)
                .send();

        // 奖 bid 1EOS 交给吃单者
        action(
                permission_level{_self, N(active)},
                ask.contract, N(transfer),
                make_tuple(_self, owner, _bid,
                           std::string("trade success"))
        ).send();

        // 将 ask 2CNY 交给下单者
        action(
                permission_level{_self, N(active)},
                bid.contract, N(transfer),
                make_tuple(_self, itr->owner, _ask,
                           std::string("trade success"))
        ).send();

        // 修改订单
        orders.modify(itr, 0, [&](auto &o) {
            o.bid.amount -= _ask.amount;
            o.ask.amount -= _bid.amount;
        });
    }
}

// 撤单
void eosotcbackup::retrieve(account_name owner, uint64_t order_id, extended_asset ask) {
    order_index orders(_self, ask.contract);
    auto itr = orders.find(order_id);
    eosio_assert(itr != orders.end(), "order is not exist.");
    eosio_assert(itr->owner == owner, "not the owner.");

    action(permission_level{_self, N(active)},
           _self, N(receipt), *itr
    ).send();

    asset _bid = itr->bid;

    // 返还下单给他
    action(
            permission_level{_self, N(active)},
            itr->bid.contract, N(transfer),
            make_tuple(_self, owner, _bid,
                       std::string("order retrieve"))
    ).send();
    orders.erase(itr);
}

// memo [ask,0.5000 HPY,happyeosslot]  下单. happyeosslot 是HPY的合约地址
// memo [take,0.5000 HPY,happyeosslot,id]
void eosotcbackup::onTransfer(account_name from, account_name to, extended_asset bid, std::string memo) {

    if (to != _self) return;

    require_auth(from);
    eosio_assert(bid.is_valid(), "invalid token transfer");
    eosio_assert(bid.amount > 0, "must bid a positive amount");
    if (memo.substr(0, 3) == "ask") {  // 下单
        memo.erase(0, 4);
        std::size_t p = memo.find(',');
        std::size_t f = memo.find('.');
        std::size_t s = memo.find(' ');

        extended_asset _ask;
        _ask.amount = string_to_price(memo.substr(0, s)); // 获取数量
        _ask.symbol = string_to_symbol(s-f-1, memo.substr(s+1, s-p-1).c_str());

        eosio_assert(_ask.is_valid(), "invalid token in ask");
        eosio_assert(_ask.amount > 0, "must ask a positive amount");

        eosio_assert(bid.is_valid(), "invalid token in bid");
        eosio_assert(bid.amount > 0, "must bid a positive amount");

        memo.erase(0, p+1);
        auto issuer = string_to_name(memo.c_str());
        _ask.contract = issuer;  // 发起者
        ask(from, bid, _ask);
    } else if (memo.substr(0, 4) == "take"){ // 吃单
        memo.erase(0, 5);

        // memo
        // take,1.0000 HPY,happyeosslot,7   // 要吃哪个单子

        std::size_t p = memo.find(',');
        std::size_t f = memo.find('.');
        std::size_t s = memo.find(' ');

        extended_asset _ask;
        _ask.amount = string_to_price(memo.substr(0, s));
        _ask.symbol = string_to_symbol(s-f-1, memo.substr(s+1, p-s-1).c_str());

        eosio_assert(_ask.is_valid(), "invalid token transfer");
        eosio_assert(_ask.amount > 0, "must ask a positive amount");
        memo.erase(0, p+1);

        p = memo.find(',');


        auto issuer = string_to_name(memo.substr(0, p).c_str());
        _ask.contract = issuer;
        memo.erase(0, p+1);
        auto id = string_to_price(memo);
        take(from, id, bid, _ask);
    }
}