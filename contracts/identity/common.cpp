#include "common.hpp"

#include <eosiolib/chain.h>

namespace identity {

    // 账户 trusted 是否被账户 by 信任
   bool identity_base::is_trusted_by( account_name trusted, account_name by ) {
      trust_table t( _self, by );
      return t.find( trusted ) != t.end();
   }

   // 账户是否可信任
   bool identity_base::is_trusted( account_name acnt ) {
      account_name active_producers[21];
      auto active_prod_size = get_active_producers( active_producers, sizeof(active_producers) );
      auto count = active_prod_size / sizeof(account_name);
      //如果是正在轮班的生产者之一，则信任他
      for( size_t i = 0; i < count; ++i ) {
         if( active_producers[i] == acnt )
            return true;
      }
      // 如果被正在轮班的生产者信任了，则我们也信任它
      for( size_t i = 0; i < count; ++i ) {
         if( is_trusted_by( acnt, active_producers[i] ) )
            return true;
      }
      return false;
   }

}
