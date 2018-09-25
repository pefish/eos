/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <asserter/asserter.hpp> /// defines assert_def struct (abi)

using namespace asserter;

static int global_variable = 45;

extern "C" {
    /// The apply method implements the dispatch of events to this contract
   void apply( uint64_t receiver, uint64_t code, uint64_t action ) {
       // eosio向asserter转账，打印 receiver: asserter code: eosio.token action: transfer
       eosio::print("receiver: ", eosio::name{receiver}, " code: ", eosio::name{code}, " action: ", eosio::name{action}, "\n");
       require_auth(code); // 检查签名中有没有一个是 code 的私钥签名的
       eosio::print("1\n");
       if( code == N(asserter) ) {
           eosio::print("2\n");
          if( action == N(procassert) ) {
              eosio::print("3\n");
             assertdef def = eosio::unpack_action_data<assertdef>();
              eosio::print("5", def.condition, def.message);
             // maybe assert?
             eosio_assert((uint32_t)def.condition, def.message.c_str());
          } else if( action == N(provereset) ) {
              eosio::print("4\n");
             eosio_assert(global_variable == 45, "Global Variable Initialized poorly");
             global_variable = 100;  // 每次执行，global_variable都会释放掉，下次调用仍然是45
          }
       }
    }
}
