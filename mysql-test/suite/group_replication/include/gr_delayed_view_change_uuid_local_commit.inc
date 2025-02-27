################################################################################
# Validate that View_change_log_event can be logged in a different order from
# the group communication order without breaking the group transactions ids
# generation.
#
# Steps:
#  1. Create a group with 3 members.
#  2. Start 2 servers.
#     Only setup recovery_user on server2 for force server3
#     to recover from it.
#  3. Stop applier on server 2 to force the delayed
#     View_change_log_event log to binary log.
#  4. Start Group Replication on server3.
#     It will recover from server2 and will stay on
#     RECOVERING state waiting for View_change_log_event log.
#  5. Execute 2 transactions on server1 and 2 while server3
#     is on RECOVERING.
#     Transaction on server2 will validate the
#     group_replication_gtid_assignment_block_size option.
#  6. Server 3 will apply INSERT (2) through recovery.
#     INSERT(1) will only be applied after member is ONLINE.
#  7. Start applier on server2.
#     Server3 state will change to ONLINE.
#  8. Sync all group members.
#  9. Execute more transactions on all members.
#     Transactions on server2 and 3 will validate the
#     group_replication_gtid_assignment_block_size option.
# 10. Sync all group members.
# 11. Clean up.
#
# ==== Usage ====
#
#   --let $view_change_uuid = AUTOMATIC | UUID
#   --source ../include/gr_delayed_view_change_uuid_local_commit.inc
#
# Parameters:
#   $view_change_uuid
#     The group_replication_view_change_uuid parameter for Group Replication
################################################################################
--source include/big_test.inc
--source include/have_group_replication_plugin.inc

--echo
--echo ############################################################
--echo #  1. Create a group with 3 members.
--let $rpl_skip_group_replication_start= 1
--let $rpl_server_count= 3
--source include/group_replication.inc

--let $recovery_user= recovery_user
--let $recovery_password= recovery_password
--let $vcle_seq= Gtid # Query/BEGIN # View_change # Query/COMMIT
--let $gtid_assignment_block_size = `SELECT @@GLOBAL.group_replication_gtid_assignment_block_size;`

--echo
--echo ############################################################
--echo #  2. Start 2 servers.
--echo #     Only setup recovery_user on server2 for force server3
--echo #     to recover from it.
--let $rpl_connection_name= server1
--source include/rpl_connection.inc

SET sql_log_bin = 0;
--source include/gtid_utils.inc
SET sql_log_bin = 1;

SET @group_replication_view_change_uuid_save= @@GLOBAL.group_replication_view_change_uuid;
--eval SET @@GLOBAL.group_replication_view_change_uuid = "$view_change_uuid"

--source include/start_and_bootstrap_group_replication.inc
CREATE TABLE t1 (c1 INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO t1 VALUES (0);

--let $rpl_connection_name= server2
--source include/rpl_connection.inc
--let $server2_uuid= query_get_value(SELECT @@SERVER_UUID, @@SERVER_UUID, 1)

SET sql_log_bin = 0;
--source include/gtid_utils.inc
SET sql_log_bin = 1;

SET SESSION sql_log_bin= 0;
--eval CREATE USER "$recovery_user" IDENTIFIED BY "$recovery_password"
--eval GRANT REPLICATION SLAVE ON *.* TO "$recovery_user"
FLUSH PRIVILEGES;
SET SESSION sql_log_bin= 1;

SET @group_replication_view_change_uuid_save= @@GLOBAL.group_replication_view_change_uuid;
--eval SET @@GLOBAL.group_replication_view_change_uuid = "$view_change_uuid"

--source include/start_group_replication.inc


--echo
--echo ############################################################
--echo #  3. Stop applier on server 2 to force the delayed
--echo #     View_change_log_event log to binary log.
--let $rpl_connection_name= server2
--source include/rpl_connection.inc

STOP SLAVE SQL_THREAD FOR CHANNEL "group_replication_applier";
--let $wait_condition= SELECT service_state="OFF" FROM performance_schema.replication_applier_status WHERE channel_name="group_replication_applier"
--source include/wait_condition.inc


--echo
--echo ############################################################
--echo #  4. Start Group Replication on server3.
--echo #     It will recover from server2 and will stay on
--echo #     RECOVERING state waiting for View_change_log_event log.
--let $rpl_connection_name= server3
--source include/rpl_connection.inc


SET sql_log_bin = 0;
--source include/gtid_utils.inc
SET sql_log_bin = 1;

SET SESSION sql_log_bin= 0;
call mtr.add_suppression("There was an error when connecting to the donor server.*");
call mtr.add_suppression("For details please check performance_schema.replication_connection_status table and error log messages of Slave I/O for channel group_replication_recovery.");
SET SESSION sql_log_bin= 1;

--disable_warnings
--eval CHANGE REPLICATION SOURCE TO SOURCE_USER= '$recovery_user', SOURCE_PASSWORD= '$recovery_password' FOR CHANNEL 'group_replication_recovery'
--enable_warnings

SET @group_replication_view_change_uuid_save= @@GLOBAL.group_replication_view_change_uuid;
--eval SET @@GLOBAL.group_replication_view_change_uuid = "$view_change_uuid";

--let $group_replication_start_member_state= RECOVERING
--source include/start_group_replication.inc

# Wait for server2 as donor
--let $wait_condition= SELECT source_uuid="$server2_uuid" FROM performance_schema.replication_connection_status WHERE channel_name="group_replication_recovery" AND service_state="ON"
--source include/wait_condition.inc


--echo
--echo ############################################################
--echo #  5. Execute 2 transactions on server1 and 1 while server3
--echo #     is on RECOVERING.
--echo #     Transaction on server2 will validate the
--echo #     group_replication_gtid_assignment_block_size option.
--let $rpl_connection_name= server1
--source include/rpl_connection.inc
INSERT INTO t1 VALUES (1);

--let $rpl_connection_name= server2
--source include/rpl_connection.inc
INSERT INTO t1 VALUES (2);

# View change (s1) # Create table # Insert # View change (s2) # Insert
--let $event_sequence= $vcle_seq # !Gtid_transaction # !Gtid_transaction # $vcle_seq # !Gtid_transaction
--source include/assert_binlog_events.inc


--echo
--echo ############################################################
--echo #  6. Server 3 will apply INSERT (2) through recovery.
--echo #     INSERT(1) will only be applied after member is ONLINE.
--let $rpl_connection_name= server3
--source include/rpl_connection.inc

# Wait for INSERT (2) to reach server 3 as GNO 7

if ($view_change_uuid == AUTOMATIC) {
  --let $expected_gtid_set= $group_replication_group_name:1-4:7
  if ($gtid_assignment_block_size == 1000000)
  {
    --let $expected_gtid_set= $group_replication_group_name:1-4:1000005
  }
}

if ($view_change_uuid != AUTOMATIC) {
  --let $expected_gtid_set= $group_replication_group_name:1-2:4, $view_change_uuid:1-2
  if ($gtid_assignment_block_size == 1000000)
  {
    --let $expected_gtid_set= $group_replication_group_name:1-2:1000003, $view_change_uuid:1-2
  }
}


--disable_query_log
--disable_result_log
--eval SELECT WAIT_FOR_EXECUTED_GTID_SET("$expected_gtid_set")
--enable_query_log
--enable_result_log

# View change (s1) # Create table # Insert # View change (s2) # Insert
--let $event_sequence= $vcle_seq # !Gtid_transaction # !Gtid_transaction # $vcle_seq # !Gtid_transaction
--source include/assert_binlog_events.inc

--let $assert_text= 'There is no value 1 in table t1'
--let $assert_cond= [SELECT COUNT(*) AS count FROM t1 WHERE c1=1, count, 1] = 0
--source include/assert.inc

--let $assert_text= 'There is a value 2 in table t1'
--let $assert_cond= [SELECT COUNT(*) AS count FROM t1 WHERE c1=2, count, 1] = 1
--source include/assert.inc


--echo
--echo ############################################################
--echo #  7. Start applier on server2.
--echo #     Server3 state will change to ONLINE.
--let $rpl_connection_name= server2
--source include/rpl_connection.inc
START SLAVE SQL_THREAD FOR CHANNEL "group_replication_applier";
--let $wait_condition= SELECT service_state="ON" FROM performance_schema.replication_applier_status WHERE channel_name="group_replication_applier"
--source include/wait_condition.inc

--let $rpl_connection_name= server3
--source include/rpl_connection.inc
--let $group_replication_member_state= ONLINE
--source include/gr_wait_for_member_state.inc


--echo
--echo ############################################################
--echo #  8. Sync all group members.
--let $rpl_connection_name= server1
--source include/rpl_connection.inc

if ($view_change_uuid == AUTOMATIC) {
  --let $expected_gtid_set= $group_replication_group_name:1-7
  if ($gtid_assignment_block_size == 1000000)
  {
    --let $expected_gtid_set= $group_replication_group_name:1-6:1000005
  }
}

if ($view_change_uuid != AUTOMATIC) {
  --let $expected_gtid_set= $group_replication_group_name:1-3, $view_change_uuid:1-3
  if ($gtid_assignment_block_size == 1000000)
  {
    --let $expected_gtid_set= $group_replication_group_name:1-3:1000003, $view_change_uuid:1-3
  }
}
--disable_query_log
--disable_result_log
--eval SELECT WAIT_FOR_EXECUTED_GTID_SET("$expected_gtid_set")
--enable_query_log
--enable_result_log

--let $rpl_connection_name= server2
--source include/rpl_connection.inc
--disable_query_log
--disable_result_log
--eval SELECT WAIT_FOR_EXECUTED_GTID_SET("$expected_gtid_set")
--enable_query_log
--enable_result_log

--let $rpl_connection_name= server3
--source include/rpl_connection.inc
--disable_query_log
--disable_result_log
--eval SELECT WAIT_FOR_EXECUTED_GTID_SET("$expected_gtid_set")
--enable_query_log
--enable_result_log

# View change (s1) # Create table # Insert # View change (s2) # Insert # View change (s3) # Insert
--let $event_sequence= $vcle_seq # !Gtid_transaction # !Gtid_transaction # $vcle_seq # !Gtid_transaction # $vcle_seq # !Gtid_transaction
--source include/assert_binlog_events.inc


--echo
--echo ############################################################
--echo #  9. Execute more transactions on all members.
--echo #     Transactions on server2 and 3 will validate the
--echo #     group_replication_gtid_assignment_block_size option.
--let $rpl_connection_name= server1
--source include/rpl_connection.inc
INSERT INTO t1 VALUES (11);

--let $rpl_connection_name= server2
--source include/rpl_connection.inc
INSERT INTO t1 VALUES (22);

--let $rpl_connection_name= server3
--source include/rpl_connection.inc
INSERT INTO t1 VALUES (33);
--source include/rpl_sync.inc


--echo
--echo ############################################################
--echo # 10. Sync all group members.
--let $rpl_connection_name= server1
--source include/rpl_connection.inc

if ($view_change_uuid == AUTOMATIC) {
  --let $expected_gtid_set= $group_replication_group_name:1-10
  if ($gtid_assignment_block_size == 1000000)
  {
    --let $expected_gtid_set= $group_replication_group_name:1-7:1000005-1000006:2000005
  }
}

if ($view_change_uuid != AUTOMATIC) {
  --let $expected_gtid_set= $group_replication_group_name:1-7, $view_change_uuid:1-3
  if ($gtid_assignment_block_size == 1000000)
  {
    --let $expected_gtid_set= $group_replication_group_name:1-4:1000003-1000004:2000003, $view_change_uuid:1-3
  }
}
--let $assert_text= GTID_EXECUTED must contain all committed GTIDs
--let $assert_cond= GTID_IS_EQUAL(@@GLOBAL.GTID_EXECUTED, "$expected_gtid_set")
--source include/assert.inc

--let $assert_text= 'There are 6 values in table t1'
--let $assert_cond= [SELECT COUNT(*) AS count FROM t1, count, 1] = 6
--source include/assert.inc

--let $rpl_connection_name= server2
--source include/rpl_connection.inc
--let $assert_text= GTID_EXECUTED must contain all committed GTIDs
--let $assert_cond= GTID_IS_EQUAL(@@GLOBAL.GTID_EXECUTED, "$expected_gtid_set")
--source include/assert.inc

--let $assert_text= 'There are 6 values in table t1'
--let $assert_cond= [SELECT COUNT(*) AS count FROM t1, count, 1] = 6
--source include/assert.inc

--let $rpl_connection_name= server3
--source include/rpl_connection.inc
--let $assert_text= GTID_EXECUTED must contain all committed GTIDs
--let $assert_cond= GTID_IS_EQUAL(@@GLOBAL.GTID_EXECUTED, "$expected_gtid_set")
--source include/assert.inc

--let $assert_text= 'There are 6 values in table t1'
--let $assert_cond= [SELECT COUNT(*) AS count FROM t1, count, 1] = 6
--source include/assert.inc


--echo
--echo ############################################################
--echo # 11. Clean up.
--let $rpl_connection_name= server2
--source include/rpl_connection.inc
DROP TABLE t1;

SET SESSION sql_log_bin= 0;
--eval DROP USER "$recovery_user"
SET SESSION sql_log_bin= 1;

--source include/stop_group_replication.inc
SET @@GLOBAL.group_replication_view_change_uuid= @group_replication_view_change_uuid_save;

SET sql_log_bin = 0;
--source include/gtid_utils_end.inc
SET sql_log_bin = 1;

--let $rpl_connection_name= server1
--source include/rpl_connection.inc

--source include/stop_group_replication.inc
SET @@GLOBAL.group_replication_view_change_uuid= @group_replication_view_change_uuid_save;

SET sql_log_bin = 0;
--source include/gtid_utils_end.inc
SET sql_log_bin = 1;

--let $rpl_connection_name= server3
--source include/rpl_connection.inc

--source include/stop_group_replication.inc
SET @@GLOBAL.group_replication_view_change_uuid= @group_replication_view_change_uuid_save;

SET sql_log_bin = 0;
--source include/gtid_utils_end.inc
SET sql_log_bin = 1;

--source include/group_replication_end.inc
