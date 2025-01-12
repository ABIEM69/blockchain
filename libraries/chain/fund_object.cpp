#include <graphene/chain/fund_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>
#include <boost/range.hpp>

#include <iostream>
#include <iomanip>

namespace graphene { namespace chain {

double fund_object::get_rate_percent(const fund_options::fund_rate& fr_item, const database& db) const
{
   int days_passed = (db.head_block_time().sec_since_epoch() - prev_maintenance_time_on_creation.sec_since_epoch()) / 86400;

   // the further away, the 'result' is lower
   double result = get_bonus_percent(fr_item.day_percent) - (get_bonus_percent(rates_reduction_per_month) / (double)30 * (double)(days_passed - 1));

   if (result < 0) {
      result = 0;
   }

   return result;
}

optional<fund_options::fund_rate>
fund_object::get_max_fund_rate(const share_type& fund_balance) const
{
   optional<fund_options::fund_rate> result;
   if (fund_rates.size() == 0) return { result };

   // we need nearest (to the fund_balance) and maximum values
//      // suspicious variant, right?
//      const auto& iter = std::max_element(fund_rates.begin(), fund_rates.end(),
//         [&fund_balance](const fund_options::fund_rate& item1, const fund_options::fund_rate& item2) {
//            return ( (fund_balance < item1.amount) || ((fund_balance >= item2.amount) && (item2.amount > item1.amount)) );
//         });
//      if ( (iter != fund_rates.end()) && (fund_balance >= iter->amount) ) {
//         result = *iter;
//      }

   int max_idx = -1;
   for (uint32_t i = 0; i < fund_rates.size(); ++i)
   {
      if (fund_balance >= fund_rates[i].amount)
      {
         if (max_idx == -1) {
            max_idx = i;
         }
         else
         {
            if (fund_rates[i].amount > fund_rates[max_idx].amount) {
               max_idx = i;
            }
         }
      }
   }

   if (max_idx > -1) {
      result = fund_rates[max_idx];
   }

   return result;
}

optional<fund_options::payment_rate>
fund_object::get_payment_rate(uint32_t period) const
{
   optional<fund_options::payment_rate> result;

   auto iter = std::find_if(payment_rates.begin(), payment_rates.end(), [&period](const fund_options::payment_rate& item) {
      return (item.period == period);
   });
   if (iter != payment_rates.end()) {
      result = *iter;
   }

   return result;
}

double fund_object::get_bonus_percent(uint32_t percent) const {
   return percent / 100000.0;
}

void fund_object::process(database& db) const
{
   const dynamic_global_property_object& dpo = db.get_dynamic_global_properties();
   const global_property_object& gpo = db.get_global_properties();

   auto asset_itr = db.get_index_type<asset_index>().indices().get<by_id>().find(asset_id);
   assert(asset_itr != db.get_index_type<asset_index>().indices().get<by_id>().end());
   const chain::asset_object& asst = *asset_itr;

   transaction_evaluation_state eval(&db);

   // all payments to users
   share_type daily_payments_without_owner;

   /**
    * create tmp object, because below will be reducing of fund's balance if deposit is
    * overdued (in deposits loop)
    */
   share_type old_balance = balance;

   fund_history_object::history_item h_item;
   h_item.create_datetime = db.head_block_time();

   const auto& users_idx = db.get_index_type<account_index>().indices().get<by_id>();
   std::vector<object_id_type> deps_to_remove;

   // find own fund deposits
   auto range = db.get_index_type<fund_deposit_index>().indices().get<by_fund_id>().equal_range(get_id());
   std::for_each(range.first, range.second, [&](const fund_deposit_object& dep)
   {
      if (dep.enabled)
      {
         // payment to depositor
         const optional<fund_options::payment_rate>& p_rate = get_payment_rate(dep.period);
         if (p_rate.valid())
         {
            double percent_per_day = get_bonus_percent(dep.percent) / p_rate->period;

            share_type quantity = std::roundl(percent_per_day * (long double)dep.amount.value);
            if (quantity.value > 0)
            {
               const asset& asst_quantity = db.check_supply_overflow(asst.amount(quantity));
               if (asst_quantity.amount.value > 0)
               {
                  chain::fund_payment_operation op;
                  op.issuer = asst.issuer;
                  op.fund_id = get_id();
                  op.deposit_id = dep.get_id();
                  op.asset_to_issue = asst_quantity;
                  op.issue_to_account = dep.account_id;

                  try
                  {
                     op.validate();
                     db.apply_operation(eval, op);
                  } catch (fc::assert_exception& e) { }

                  daily_payments_without_owner += asst_quantity.amount;
               }
            }
         }

         // return deposit amount to user and remove deposit if overdue
         if ((dpo.next_maintenance_time - gpo.parameters.maintenance_interval) >= dep.datetime_end)
         {
            bool dep_was_overdue = true;

            if (db.head_block_time() >= HARDFORK_624_TIME)
            {
               auto itr_user = users_idx.find(dep.account_id);
               if (itr_user != users_idx.end())
               {
                  const account_object& acc = *itr_user;
                  if (acc.deposits_autorenewal_enabled)
                  {
                     dep_was_overdue = false;

                     if (db.head_block_time() > HARDFORK_625_TIME)
                     {
                        chain::deposit_renewal_operation op;
                        op.account_id = dep.account_id;
                        op.deposit_id = dep.get_id();
                        op.percent = dep.percent;
                        if (p_rate.valid()) {
                           op.percent = p_rate->percent;
                        }
                        op.datetime_end = dep.datetime_end + (86400 * dep.period);

                        try
                        {
                           op.validate();
                           db.apply_operation(eval, op);
                        } catch (fc::assert_exception& e) { }
                     }
                     // last_budget_time - not stable
                     else
                     {
                        db.modify(dep, [&](fund_deposit_object& dep)
                        {
                           if (p_rate.valid()) {
                              dep.percent = p_rate->percent;
                           }
                           dep.datetime_end = db.get_dynamic_global_properties().last_budget_time + (86400 * dep.period);
                        });
                     };
                  }
               }
            }

            if (dep_was_overdue)
            {
               // remove deposit
               deps_to_remove.emplace_back(dep.get_id());

               // return deposit to user
               chain::fund_withdrawal_operation op;
               op.issuer = asst.issuer;
               op.fund_id = get_id();
               op.asset_to_issue = asst.amount(dep.amount);
               op.issue_to_account = dep.account_id;
               op.datetime         = db.head_block_time();

               try
               {
                  op.validate();
                  db.apply_operation(eval, op);
               } catch (fc::assert_exception& e) { }

               // reduce fund balance
               db.modify(*this, [&](chain::fund_object& f) {
                  f.balance -= dep.amount;
               });

               // disable deposit
               db.modify(dep, [&](chain::fund_deposit_object& f) {
                  f.enabled = false;
               });
            }
         }
      }
   });

   // make payment to fund owner, variant 1
   if (fixed_percent_on_deposits > 0)
   {
      share_type quantity = std::roundl(get_bonus_percent(fixed_percent_on_deposits) * (long double)daily_payments_without_owner.value);
      const asset& asst_owner_quantity = db.check_supply_overflow(asst.amount(quantity));

      if (asst_owner_quantity.amount.value > 0)
      {
         chain::fund_payment_operation op;
         op.issuer = asst.issuer;
         op.fund_id = get_id();
         op.asset_to_issue = asst_owner_quantity;
         op.issue_to_account = owner;

         try
         {
            op.validate();
            db.apply_operation(eval, op);
         } catch (fc::assert_exception& e) { }
      }
   }
   // make payment to fund owner, variant 2
   else
   {
      const optional<fund_options::fund_rate>& p_rate = get_max_fund_rate(old_balance);
      if (p_rate.valid())
      {
         share_type fund_day_profit = std::roundl((long double)old_balance.value * get_rate_percent(*p_rate, db));
         if (fund_day_profit > 0)
         {
            h_item.daily_profit = fund_day_profit;
            h_item.daily_payments_without_owner = daily_payments_without_owner;

            share_type owner_profit = fund_day_profit - daily_payments_without_owner;
            const asset& asst_owner_quantity = db.check_supply_overflow(asst.amount(owner_profit));

            // std::cout << "old_balance: " << old_balance.value
            //           << ", fund_deposits_sum: " << fund_deposits_sum.value
            //           << ", owner profit: " << owner_profit.value << std::endl;

            if (asst_owner_quantity.amount.value > 0)
            {
               chain::fund_payment_operation op;
               op.issuer = asst.issuer;
               op.fund_id = get_id();
               op.asset_to_issue = asst_owner_quantity;
               op.issue_to_account = owner;

               try
               {
                  op.validate();
                  db.apply_operation(eval, op);
               } catch (fc::assert_exception& e) { }
            }
         }
      }
   }

   // erase overdue deposits if no full-node
   if (db.get_history_size() > 0)
   {
      for (const object_id_type& obj_id: deps_to_remove) {
         db.remove(db.get_object(obj_id));
      }
   }

   // erase old history items
   const auto& hist_obj = history_id(db);
   db.modify(hist_obj, [&](fund_history_object& o)
   {
      o.items.emplace_back(std::move(h_item));

      if (db.get_history_size() > 0)
      {
         const time_point& tp = db.head_block_time() - fc::days(db.get_history_size());

         for (auto it = o.items.begin(); it != o.items.end();)
         {
            if (it->create_datetime < tp) {
               it = o.items.erase(it);
            }
            else {
               ++it;
            }
         }
      }
   });
}

void fund_object::finish(database& db) const
{
   share_type owner_deps = owner_balance;
   if (owner_deps > 0)
   {
      auto asset_itr = db.get_index_type<asset_index>().indices().get<by_id>().find(asset_id);
      assert(asset_itr != db.get_index_type<asset_index>().indices().get<by_id>().end());
      const chain::asset_object& asst = *asset_itr;
      transaction_evaluation_state eval(&db);

      // return amount to owner
      chain::fund_withdrawal_operation op;
      op.issuer           = asst.issuer;
      op.fund_id          = get_id();
      op.asset_to_issue   = asst.amount(owner_deps);
      op.issue_to_account = owner;
      op.datetime         = db.head_block_time();

      try
      {
         op.validate();
         db.apply_operation(eval, op);
      } catch (fc::assert_exception& e) { }
   }

   // reduce fund balance & disable
   db.modify(*this, [&](chain::fund_object& f)
   {
      if (owner_deps > 0) {
         f.balance -= owner_deps;
      }
      f.owner_balance = 0;
      f.enabled = false;
   });
}

} } // graphene::chain
