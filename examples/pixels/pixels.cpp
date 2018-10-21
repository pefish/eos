#include "EOSPixels.hpp"

#include <cmath>
#include <eosiolib/action.hpp>
#include <eosiolib/asset.hpp>

#include "memo.hpp"
#include "types.hpp"

using namespace eosio;
using namespace std;

template <uint64_t A, typename B, typename... C>
void clear_table(multi_index<A, B, C...> *table, uint16_t limit) {
    auto it = table->begin();
    uint16_t count = 0;
    while (it != table->end() && count < limit) {
        it = table->erase(it);
        count++;
    }
}

// 清除前面 count 个像素
void eospixels::clearpixels(uint16_t count, uint16_t nonce) {
    require_auth(TEAM_ACCOUNT);

    auto itr = canvases.begin();
    eosio_assert(itr != canvases.end(), "no canvas exists");

    pixel_store pixels(_self, itr->id);
    clear_table(&pixels, count);
}

// 清除前面 count 个账户
void eospixels::clearaccts(uint16_t count, uint16_t nonce) {
    require_auth(TEAM_ACCOUNT);

    clear_table(&accounts, count);
}

// 清除前面 count 张画布
void eospixels::clearcanvs(uint16_t count, uint16_t nonce) {
    require_auth(TEAM_ACCOUNT);

    clear_table(&canvases, count);
}

// 重置提现限额
void eospixels::resetquota() {
    require_auth(TEAM_ACCOUNT);

    auto guardItr = guards.begin();
    if (guardItr == guards.end()) {
        guards.emplace(_self, [&](guard &grd) {
            grd.id = 0;
            grd.quota = WITHDRAW_QUOTA;
        });
    } else {
        guards.modify(guardItr, 0, [&](guard &grd) { grd.quota = WITHDRAW_QUOTA; });
    }
}

// 购买单个像素
void eospixels::drawPixel(pixel_store &allPixels,
                          const st_pixelOrder &pixelOrder,
                          st_transferContext &ctx) {
    auto loc = pixelOrder.location();

    auto pixelRowItr = allPixels.find(loc.row);

    // TODO extract this into its own method
    // Emplace & initialize empty row if it doesn't already exist
    bool hasRow = pixelRowItr != allPixels.end();
    if (!hasRow) { // 这一行没被写过，就创建这一行
        pixelRowItr = allPixels.emplace(_self, [&](pixel_row &pixelRow) {
            pixelRow.row = loc.row;
            pixelRow.initialize_empty_pixels();
        });
    }

    auto pixels = pixelRowItr->pixels;
    auto pixel = pixels[loc.col]; // 找到这个像素点

    auto result = ctx.purchase(pixel, pixelOrder); // 购买这个像素点
    if (result.isSkipped) {
        return;
    }

    // 更新表
    allPixels.modify(pixelRowItr, 0, [&](pixel_row &pixelRow) {
        pixelRow.pixels[loc.col] = {pixelOrder.color, pixel.nextPriceCounter(),
                                    ctx.purchaser};
    });

    // 如果不是新的像素点，则把本金及收益退给上一个购买者(退到游戏钱包，不是直接打给账户)
    if (!result.isFirstBuyer) {
        deposit(pixel.owner, result.ownerEarningScaled);
    }
}

bool eospixels::isValidReferrer(account_name name) {
    auto it = accounts.find(name);

    if (it == accounts.end()) {
        return false;
    }

    // referrer must have painted at least one pixel
    return it->pixelsDrawn > 0;
}

// h7ockw1udj3,er2ofll0kjj,eqzk6qac73z,er14h310jr3,h7rgtrciqyn,din67jj1bsv,h7pwldmygan,fzbydt63abj;tpdappincome
void eospixels::onTransfer(const currency::transfer &transfer) {
    if (transfer.to != _self) return;

    auto canvasItr = canvases.begin();
    eosio_assert(canvasItr != canvases.end(), "game not started");
    auto canvas = *canvasItr;
    eosio_assert(!canvas.isEnded(), "game ended");

    auto from = transfer.from;
    auto accountItr = accounts.find(from);
    eosio_assert(accountItr != accounts.end(), // 要求这个用户已经被创建
                 "account not registered to the game");

    pixel_store allPixels(_self, canvas.id);

    auto memo = TransferMemo();
    memo.parse(transfer.memo); // 将坐标数据解析到 memo.pixelOrders

    auto ctx = st_transferContext();
    ctx.amountLeft = transfer.quantity.amount;
    ctx.purchaser = transfer.from; // 购买者
    ctx.referrer = memo.referrer;

    // Remove referrer if it is invalid
    if (ctx.referrer != 0 &&
        (ctx.referrer == from || !isValidReferrer(ctx.referrer))) {
        ctx.referrer = 0;
    }

    // Every pixel has a "fee". For IPO the fee is the whole pixel price. For
    // takeover, the fee is a percentage of the price increase.
    // 购买所有用户要购买的像素
    for (auto &pixelOrder : memo.pixelOrders) {
        drawPixel(allPixels, pixelOrder, ctx);
    }

    size_t paintSuccessPercent =
            ctx.paintedPixelCount * 100 / memo.pixelOrders.size();
    eosio_assert(paintSuccessPercent >= 80, "Too many pixels did not paint.");

    // 如果剩下的钱还有多的，则放进余额
    if (ctx.amountLeft > 0) {
        // Refund user with whatever is left over
        deposit(from, ctx.amountLeftScaled());
    }

    ctx.updateFeesDistribution(); // 分发fee到各个变量

    // 更新画布
    canvases.modify(canvasItr, 0, [&](auto &cv) {
        cv.lastPaintedAt = now();
        cv.lastPainter = from;

        ctx.updateCanvas(cv);
    });

    // 更新账户
    accounts.modify(accountItr, 0,
                    [&](account &acct) { ctx.updatePurchaserAccount(acct); });

    if (ctx.hasReferrer()) { // 如果有邀请人的话，给邀请人奖励
        deposit(ctx.referrer, ctx.referralEarningScaled);
    }
}

// 结束已过期画布，开启新画布
void eospixels::end() {
    // anyone can create new canvas
    auto itr = canvases.begin();
    eosio_assert(itr != canvases.end(), "no canvas exists");

    auto c = *itr;
    eosio_assert(c.isEnded(), "canvas still has time left"); // 要求画布已经过期

    // reclaim memory
    canvases.erase(itr); // 删除画布，释放内存

    // create new canvas
    canvases.emplace(_self, [&](canvas &newCanvas) {
        newCanvas.id = c.id + 1;
        newCanvas.lastPaintedAt = now();
        newCanvas.duration = CANVAS_DURATION;
    });
}

void eospixels::refreshLastPaintedAt() {
    auto itr = canvases.begin();
    eosio_assert(itr != canvases.end(), "no canvas exists");

    canvases.modify(itr, 0,
                    [&](canvas &newCanvas) { newCanvas.lastPaintedAt = now(); });
}

void eospixels::refresh() {
    require_auth(TEAM_ACCOUNT); // 项目方才能调用

    refreshLastPaintedAt();
}

// 修改画布有效期(项目方才能调用)
void eospixels::changedur(time duration) {
    require_auth(TEAM_ACCOUNT);

    auto itr = canvases.begin();
    eosio_assert(itr != canvases.end(), "no canvas exists");

    canvases.modify(itr, 0,
                    [&](canvas &newCanvas) { newCanvas.duration = duration; });
}

// 创建账户
void eospixels::createacct(const account_name account) {
    require_auth(account);

    auto itr = accounts.find(account);
    eosio_assert(itr == accounts.end(), "account already exist");

    accounts.emplace(account, [&](auto &acct) { acct.owner = account; });
}

void eospixels::init() {
    require_auth(_self);
    // make sure table records is empty
    eosio_assert(canvases.begin() == canvases.end(), "already initialized");
    // 初始化一张画布
    canvases.emplace(_self, [&](canvas &newCanvas) {
        newCanvas.id = 0;
        newCanvas.lastPaintedAt = now();
        newCanvas.duration = CANVAS_DURATION; // 24小时有效期
    });
}

void eospixels::withdraw(const account_name to) {
    require_auth(to);

    auto canvasItr = canvases.begin();
    eosio_assert(canvasItr != canvases.end(), "no canvas exists");

    auto canvas = *canvasItr;
    eosio_assert(canvas.pixelsDrawn >= WITHDRAW_PIXELS_THRESHOLD,
                 "canvas still in game initialization");

    auto acctItr = accounts.find(to);
    eosio_assert(acctItr != accounts.end(), "unknown account");

    auto guardItr = guards.begin();
    eosio_assert(guardItr != guards.end(), "no withdraw guard exists");

    auto player = *acctItr;
    auto grd = *guardItr;

    uint64_t withdrawAmount = calculateWithdrawalAndUpdate(canvas, player, grd); // 获取可提现的金额

    guards.modify(guardItr, 0, [&](guard &g) { g.quota = grd.quota; }); // 更新限额

    // 更新账户信息
    accounts.modify(acctItr, 0, [&](account &acct) {
        acct.balanceScaled = player.balanceScaled;
        acct.maskScaled = player.maskScaled;
    });

    // 提现
    auto quantity = asset(withdrawAmount, EOS_SYMBOL);
    action(permission_level{_self, N(active)}, N(eosio.token), N(transfer),
           std::make_tuple(_self, to, quantity,
                           std::string("Withdraw from EOS Pixels")))
            .send();
}

// 给账户增加余额
void eospixels::deposit(const account_name user,
                        const uint128_t quantityScaled) {
    eosio_assert(quantityScaled > 0, "must deposit positive quantity");

    auto itr = accounts.find(user);

    accounts.modify(itr, 0,
                    [&](auto &acct) { acct.balanceScaled += quantityScaled; });
}

void eospixels::apply(account_name contract, action_name act) {
    if (contract == N(eosio.token) && act == N(transfer)) {
        // React to transfer notification.
        // DANGER: All methods MUST check whethe token symbol is acceptable.

        auto transfer = unpack_action_data<currency::transfer>();
        eosio_assert(transfer.quantity.symbol == EOS_SYMBOL,
                     "must pay with EOS token");
        onTransfer(transfer);
        return;
    }

    if (contract != _self) return;

    // needed for EOSIO_API macro
    auto &thiscontract = *this;
    switch (act) {
        // first argument is name of CPP class, not contract
        EOSIO_API(eospixels, (init)(refresh)(changedur)(end)(createacct)(withdraw)(
                clearpixels)(clearaccts)(clearcanvs)(resetquota))
    };
}

extern "C" {
[[noreturn]] void apply(uint64_t receiver, uint64_t code, uint64_t action) {
    eospixels pixels(receiver);
    pixels.apply(code, action);
    eosio_exit(0);
}
}
