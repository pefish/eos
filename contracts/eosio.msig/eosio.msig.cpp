#include <eosio.msig/eosio.msig.hpp>
#include <eosiolib/action.hpp>
#include <eosiolib/permission.hpp>

namespace eosio {

/*
propose function manually parses input data (instead of taking parsed arguments from dispatcher)
because parsing data in the dispatcher uses too much CPU in case if proposed transaction is big

If we use dispatcher the function signature should be:

void multisig::propose( account_name proposer,
                        name proposal_name,
                        vector<permission_level> requested,
                        transaction  trx)
*/

// 发起提案
void multisig::propose() {
   constexpr size_t max_stack_buffer_size = 512;
   size_t size = action_data_size(); // 获取action的data的大小
   char* buffer = (char*)( max_stack_buffer_size < size ? malloc(size) : alloca(size) );
   read_action_data( buffer, size );

   account_name proposer;
   name proposal_name;
   vector<permission_level> requested; // 表示提案中要发送的交易需要请示哪些权限
   transaction_header trx_header;

   // 获取参数
   datastream<const char*> ds( buffer, size );
   ds >> proposer >> proposal_name >> requested;

   size_t trx_pos = ds.tellp();
   ds >> trx_header;

   require_auth( proposer );
   eosio_assert( trx_header.expiration >= eosio::time_point_sec(now()), "transaction expired" );
   //eosio_assert( trx_header.actions.size() > 0, "transaction must have at least one action" );

   proposals proptable( _self, proposer );
   // 检查是否提案已经存在
   eosio_assert( proptable.find( proposal_name ) == proptable.end(), "proposal with the same name exists" );

   bytes packed_requested = pack(requested);
   // 检查交易要请求的权限是不是合理的
   auto res = ::check_transaction_authorization( buffer+trx_pos, size-trx_pos,
                                                 (const char*)0, 0,
                                                 packed_requested.data(), packed_requested.size()
                                               );
   eosio_assert( res > 0, "transaction authorization failed" );

   proptable.emplace( proposer, [&]( auto& prop ) {
      prop.proposal_name       = proposal_name;
      prop.packed_transaction  = bytes( buffer+trx_pos, buffer+size );
   });

   approvals apptable(  _self, proposer );
   apptable.emplace( proposer, [&]( auto& a ) {
      a.proposal_name       = proposal_name;
      a.requested_approvals = std::move(requested);  // 还需要请求的权限
   });
}

// 赞同提案
void multisig::approve( account_name proposer, name proposal_name, permission_level level ) {
   require_auth( level );

   approvals apptable(  _self, proposer );
   auto& apps = apptable.get( proposal_name, "proposal not found" );

   auto itr = std::find( apps.requested_approvals.begin(), apps.requested_approvals.end(), level );
   eosio_assert( itr != apps.requested_approvals.end(), "approval is not on the list of requested approvals" );

   apptable.modify( apps, proposer, [&]( auto& a ) {
      a.provided_approvals.push_back( level );  // 已经请求到的权限
      a.requested_approvals.erase( itr );
   });
}

// 取消赞同
void multisig::unapprove( account_name proposer, name proposal_name, permission_level level ) {
   require_auth( level );

   approvals apptable(  _self, proposer );
   auto& apps = apptable.get( proposal_name, "proposal not found" );
   auto itr = std::find( apps.provided_approvals.begin(), apps.provided_approvals.end(), level );
   eosio_assert( itr != apps.provided_approvals.end(), "no approval previously granted" );

   apptable.modify( apps, proposer, [&]( auto& a ) {
      a.requested_approvals.push_back(level);
      a.provided_approvals.erase(itr);
   });
}

// 取消提案
void multisig::cancel( account_name proposer, name proposal_name, account_name canceler ) {
   require_auth( canceler );

   proposals proptable( _self, proposer );
   auto& prop = proptable.get( proposal_name, "proposal not found" );

   if( canceler != proposer ) {
      eosio_assert( unpack<transaction_header>( prop.packed_transaction ).expiration < eosio::time_point_sec(now()), "cannot cancel until expiration" );
   }

   approvals apptable(  _self, proposer );
   auto& apps = apptable.get( proposal_name, "proposal not found" );

   proptable.erase(prop);
   apptable.erase(apps);
}

// 执行提案
void multisig::exec( account_name proposer, name proposal_name, account_name executer ) {
   require_auth( executer );

   proposals proptable( _self, proposer );
   auto& prop = proptable.get( proposal_name, "proposal not found" );

   approvals apptable(  _self, proposer );
   auto& apps = apptable.get( proposal_name, "proposal not found" );

   transaction_header trx_header;
   datastream<const char*> ds( prop.packed_transaction.data(), prop.packed_transaction.size() );
   ds >> trx_header;
   eosio_assert( trx_header.expiration >= eosio::time_point_sec(now()), "transaction expired" );

   bytes packed_provided_approvals = pack(apps.provided_approvals);
   auto res = ::check_transaction_authorization( prop.packed_transaction.data(), prop.packed_transaction.size(),
                                                 (const char*)0, 0,
                                                 packed_provided_approvals.data(), packed_provided_approvals.size()
                                               );
   eosio_assert( res > 0, "transaction authorization failed" );
   // 执行交易
   send_deferred( (uint128_t(proposer) << 64) | proposal_name, executer, prop.packed_transaction.data(), prop.packed_transaction.size() );

   // 清除相关记录
   proptable.erase(prop);
   apptable.erase(apps);
}

} /// namespace eosio

EOSIO_ABI( eosio::multisig, (propose)(approve)(unapprove)(cancel)(exec) )
