//
// rtidb.cc
// Copyright (C) 2017 4paradigm.com
// Author wangtaize 
// Date 2017-03-31
//
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <iostream>
#include <sstream>

#include <gflags/gflags.h>
#include <brpc/server.h>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include "logging.h"

#include "tablet/tablet_impl.h"
#include "nameserver/name_server_impl.h"
#include "client/tablet_client.h"
#include "client/ns_client.h"
#include "base/strings.h"
#include "base/kv_iterator.h"
#include "base/schema_codec.h"
#include "base/flat_array.h"
#include "timer.h"
#include "version.h"
#include "proto/tablet.pb.h"
#include "proto/client.pb.h"
#include "proto/name_server.pb.h"
#include "tprinter.h"
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

using ::baidu::common::INFO;
using ::baidu::common::WARNING;
using ::baidu::common::DEBUG;

DECLARE_string(endpoint);
DECLARE_int32(thread_pool_size);
DECLARE_int32(put_concurrency_limit);
DECLARE_int32(scan_concurrency_limit);
DECLARE_int32(get_concurrency_limit);
DEFINE_string(role, "tablet | nameserver | client | ns_client", "Set the rtidb role for start");
DEFINE_string(cmd, "", "Set the command");
DEFINE_bool(interactive, true, "Set the interactive");

DEFINE_string(log_dir, "", "Config the log dir");
DEFINE_int32(log_file_size, 1024, "Config the log size in MB");
DEFINE_int32(log_file_count, 24, "Config the log count");
DEFINE_string(log_level, "debug", "Set the rtidb log level, eg: debug or info");

void SetupLog() {
    // Config log 
    if (FLAGS_log_level == "debug") {
        ::baidu::common::SetLogLevel(DEBUG);
    }else {
        ::baidu::common::SetLogLevel(INFO);
    }
    if (!FLAGS_log_dir.empty()) {
        std::string info_file = FLAGS_log_dir + "/rtidb.info.log";
        std::string warning_file = FLAGS_log_dir + "/rtidb.warning.log";
        ::baidu::common::SetLogFile(info_file.c_str());
        ::baidu::common::SetWarningFile(warning_file.c_str());
    }
    ::baidu::common::SetLogCount(FLAGS_log_file_count);
    ::baidu::common::SetLogSize(FLAGS_log_file_size);
}

void StartNameServer() {
    SetupLog();
    ::rtidb::nameserver::NameServerImpl* name_server = new ::rtidb::nameserver::NameServerImpl();
    name_server->Init();
    brpc::ServerOptions options;
    options.num_threads = FLAGS_thread_pool_size;
    brpc::Server server;
    if (server.AddService(name_server, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        PDLOG(WARNING, "Fail to add service");
        exit(1);
    }
    if (server.Start(FLAGS_endpoint.c_str(), &options) != 0) {
        PDLOG(WARNING, "Fail to start server");
        exit(1);
    }
    PDLOG(INFO, "start nameserver on port %s with version %d.%d.%d", FLAGS_endpoint.c_str(),
            RTIDB_VERSION_MAJOR,
            RTIDB_VERSION_MINOR,
            RTIDB_VERSION_BUG);
    std::ostringstream oss;
    oss << RTIDB_VERSION_MAJOR << "." << RTIDB_VERSION_MINOR << "." << RTIDB_VERSION_BUG;
    server.set_version(oss.str());
    server.RunUntilAskedToQuit();
}

void StartTablet() {
    SetupLog();
    ::rtidb::tablet::TabletImpl* tablet = new ::rtidb::tablet::TabletImpl();
    bool ok = tablet->Init();
    if (!ok) {
        PDLOG(WARNING, "fail to init tablet");
        exit(1);
    }
    brpc::ServerOptions options;
    options.num_threads = FLAGS_thread_pool_size;
    brpc::Server server;
    if (server.AddService(tablet, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        PDLOG(WARNING, "Fail to add service");
        exit(1);
    }
    server.MaxConcurrencyOf(tablet, "Scan") = FLAGS_scan_concurrency_limit;
    server.MaxConcurrencyOf(tablet, "Put") = FLAGS_put_concurrency_limit;
    server.MaxConcurrencyOf(tablet, "Get") = FLAGS_get_concurrency_limit;
    if (server.Start(FLAGS_endpoint.c_str(), &options) != 0) {
        PDLOG(WARNING, "Fail to start server");
        exit(1);
    }
    PDLOG(INFO, "start tablet on port %s with version %d.%d.%d", FLAGS_endpoint.c_str(),
            RTIDB_VERSION_MAJOR,
            RTIDB_VERSION_MINOR,
            RTIDB_VERSION_BUG);
    std::ostringstream oss;
    oss << RTIDB_VERSION_MAJOR << "." << RTIDB_VERSION_MINOR << "." << RTIDB_VERSION_BUG;
    server.set_version(oss.str());
    server.RunUntilAskedToQuit();
}

void ShowTableRow(const std::vector<::rtidb::base::ColumnDesc>& schema, 
                  const char* row,
                  const uint32_t row_size,
                  const uint64_t ts,
                  const uint32_t index,
                  ::baidu::common::TPrinter& tp) {
    rtidb::base::FlatArrayIterator fit(row, row_size);
    std::vector<std::string> vrow;
    vrow.push_back(boost::lexical_cast<std::string>(index));
    vrow.push_back(boost::lexical_cast<std::string>(ts));
    while (fit.Valid()) {
        std::string col;
        if (fit.GetType() == ::rtidb::base::ColType::kString) {
            fit.GetString(&col);
        }else if (fit.GetType() == ::rtidb::base::ColType::kInt32) {
            int32_t int32_col = 0;
            fit.GetInt32(&int32_col);
            col = boost::lexical_cast<std::string>(int32_col);
        }else if (fit.GetType() == ::rtidb::base::ColType::kInt64) {
            int64_t int64_col = 0;
            fit.GetInt64(&int64_col);
            col = boost::lexical_cast<std::string>(int64_col);
        }else if (fit.GetType() == ::rtidb::base::ColType::kUInt32) {
            uint32_t uint32_col = 0;
            fit.GetUInt32(&uint32_col);
            col = boost::lexical_cast<std::string>(uint32_col);
        }else if (fit.GetType() == ::rtidb::base::ColType::kUInt64) {
            uint64_t uint64_col = 0;
            fit.GetUInt64(&uint64_col);
            col = boost::lexical_cast<std::string>(uint64_col);
        }else if (fit.GetType() == ::rtidb::base::ColType::kDouble) {
            double double_col = 0.0d;
            fit.GetDouble(&double_col);
            col = boost::lexical_cast<std::string>(double_col);
        }else if (fit.GetType() == ::rtidb::base::ColType::kFloat) {
            float float_col = 0.0f;
            fit.GetFloat(&float_col);
            col = boost::lexical_cast<std::string>(float_col);
        }
        fit.Next();
        vrow.push_back(col);
    }
    tp.AddRow(vrow);
}

void ShowTableRows(const std::string& schema, 
                   ::rtidb::base::KvIterator* it) { 
    std::vector<::rtidb::base::ColumnDesc> raw;
    ::rtidb::base::SchemaCodec codec;
    codec.Decode(schema, raw);
    ::baidu::common::TPrinter tp(raw.size() + 2, 128);
    std::vector<std::string> row;
    row.push_back("#");
    row.push_back("ts");
    for (uint32_t i = 0; i < raw.size(); i++) {
        row.push_back(raw[i].name);
    }
    tp.AddRow(row);
    uint32_t index = 1;
    while (it->Valid()) {
        rtidb::base::FlatArrayIterator fit(it->GetValue().data(), it->GetValue().size());
        ShowTableRow(raw, it->GetValue().data(), it->GetValue().size(), it->GetKey(), index, tp); 
        index ++;
        it->Next();
    }
    tp.Print(true);
}


void HandleNSShowTablet(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    std::vector<std::string> row;
    row.push_back("endpoint");
    row.push_back("state");
    row.push_back("age");
    ::baidu::common::TPrinter tp(row.size());
    tp.AddRow(row);
    std::vector<::rtidb::client::TabletInfo> tablets;
    std::string msg;
    bool ok = client->ShowTablet(tablets, msg);
    if (!ok) {
        std::cout << "Fail to show tablets. error msg: " << msg << std::endl;
        return;
    }
    for (size_t i = 0; i < tablets.size(); i++) { 
        std::vector<std::string> row;
        row.push_back(tablets[i].endpoint);
        row.push_back(tablets[i].state);
        row.push_back(::rtidb::base::HumanReadableTime(tablets[i].age));
        tp.AddRow(row);
    }
    tp.Print(true);
}

void HandleNSMakeSnapshot(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    try {
        uint32_t pid = boost::lexical_cast<uint32_t>(parts[2]);
        std::string msg;
        bool ok = client->MakeSnapshot(parts[1], pid, msg);
        if (!ok) {
            std::cout << "Fail to makesnapshot. error msg:" << msg << std::endl;
            return;
        }
        std::cout << "MakeSnapshot ok" << std::endl;
    } catch(std::exception const& e) {
        std::cout << "Invalid args. pid should be uint32_t" << std::endl;
    } 
}

void HandleNSAddReplica(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    try {
        uint32_t pid = boost::lexical_cast<uint32_t>(parts[2]);
        std::string msg;
        bool ok = client->AddReplica(parts[1], pid, parts[3], msg);
        if (!ok) {
            std::cout << "Fail to addreplica. error msg:" << msg  << std::endl;
            return;
        }
        std::cout << "AddReplica ok" << std::endl;
    } catch(std::exception const& e) {
        std::cout << "Invalid args. pid should be uint32_t" << std::endl;
    } 
}

void HandleNSDelReplica(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    try {
        uint32_t pid = boost::lexical_cast<uint32_t>(parts[2]);
        std::string msg;
        bool ok = client->DelReplica(parts[1], pid, parts[3], msg);
        if (!ok) {
            std::cout << "Fail to delreplica. error msg:" << msg << std::endl;
            return;
        }
        std::cout << "DelReplica ok" << std::endl;
    } catch(std::exception const& e) {
        std::cout << "Invalid args. pid should be uint32_t" << std::endl;
    } 
}
    
void HandleNSClientDropTable(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 2) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    std::string msg;
    bool ret = client->DropTable(parts[1], msg);
    if (!ret) {
        std::cout << "failed to drop. error msg: " << msg << std::endl;
        return;
    }
    std::cout << "drop ok" << std::endl;
}

void HandleNSClientConfSet(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    std::string msg;
    bool ret = client->ConfSet(parts[1], parts[2], msg);
    if (!ret) {
         printf("failed to set %s. error msg: %s\n", parts[1].c_str(), msg.c_str());
        return;
    }
    printf("set %s ok\n", parts[1].c_str());
}

void HandleNSClientConfGet(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 1) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    std::string msg;
    std::map<std::string, std::string> conf_map;
    std::string key;
    if (parts.size() > 1) {
        key = parts[1];
    }
    bool ret = client->ConfGet(key, conf_map, msg);
    if (!ret) {
         printf("failed to set %s. error msg: %s\n", parts[1].c_str(), msg.c_str());
        return;
    }
    std::vector<std::string> row;
    row.push_back("key");
    row.push_back("value");
    ::baidu::common::TPrinter tp(row.size());
    tp.AddRow(row);
    for (const auto& kv : conf_map) {
        row.clear();
        row.push_back(kv.first);
        row.push_back(kv.second);
        tp.AddRow(row);
    }
    tp.Print(true);
}

void HandleNSClientChangeLeader(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    try {
        uint32_t pid = boost::lexical_cast<uint32_t>(parts[2]);
        std::string msg;
        bool ret = client->ChangeLeader(parts[1], pid, msg);
        if (!ret) {
            std::cout << "failed to change leader. error msg: " << msg << std::endl;
            return;
        }
    } catch(const std::exception& e) {
        std::cout << "Invalid args. pid should be uint32_t" << std::endl;
        return;
    }
    std::cout << "change leader ok" << std::endl;
}   

void HandleNSClientOfflineEndpoint(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 2) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    std::string msg;
    bool ret = client->OfflineEndpoint(parts[1], msg);
    if (!ret) {
        std::cout << "failed to offline endpoint. error msg: " << msg << std::endl;
        return;
    }
    std::cout << "offline endpoint ok" << std::endl;
}

void HandleNSClientRecoverEndpoint(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 2) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    std::string msg;
    bool ret = client->RecoverEndpoint(parts[1], msg);
    if (!ret) {
        std::cout << "failed to recover endpoint. error msg: " << msg << std::endl;
        return;
    }
    std::cout << "recover endpoint ok" << std::endl;
}    

void HandleNSClientConnectZK(const std::vector<std::string> parts, ::rtidb::client::NsClient* client) {
    std::string msg;
    bool ok = client->ConnectZK(msg);
    if (ok) {
        std::cout << "connect zk ok" << std::endl;
    } else {
        std::cout << "Fail to connect zk" << std::endl;
    }
}

void HandleNSClientDisConnectZK(const std::vector<std::string> parts, ::rtidb::client::NsClient* client) {
    std::string msg;
    bool ok = client->DisConnectZK(msg);
    if (ok) {
        std::cout << "disconnect zk ok" << std::endl;
    } else {
        std::cout << "Fail to disconnect zk" << std::endl;
    }
}

void HandleNSClientShowTable(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    std::string name;
    if (parts.size() >= 2) {
        name = parts[1];
    }
    std::vector<::rtidb::nameserver::TableInfo> tables;
    std::string msg;
    bool ret = client->ShowTable(name, tables, msg);
    if (!ret) {
        std::cout << "failed to showtable. error msg: " << msg << std::endl;
        return;
    }
    std::vector<std::string> row;
    row.push_back("name");
    row.push_back("tid");
    row.push_back("pid");
    row.push_back("endpoint");
    row.push_back("role");
    row.push_back("seg_cnt");
    row.push_back("ttl");
    row.push_back("is_alive");
    ::baidu::common::TPrinter tp(row.size());
    tp.AddRow(row);
    for (const auto& value : tables) {
        for (int idx = 0; idx < value.table_partition_size(); idx++) {
            for (int meta_idx = 0; meta_idx < value.table_partition(idx).partition_meta_size(); meta_idx++) {
                row.clear();
                row.push_back(value.name());
                row.push_back(std::to_string(value.tid()));
                row.push_back(std::to_string(value.table_partition(idx).pid()));
                row.push_back(value.table_partition(idx).partition_meta(meta_idx).endpoint());
                if (value.table_partition(idx).partition_meta(meta_idx).is_leader()) {
                    row.push_back("leader");
                } else {
                    row.push_back("follower");
                }
                row.push_back(std::to_string(value.seg_cnt()));
                row.push_back(std::to_string(value.ttl()));
                if (value.table_partition(idx).partition_meta(meta_idx).is_alive()) {
                    row.push_back("yes");
                } else {
                    row.push_back("no");
                }
                tp.AddRow(row);
            }
        }
    }
    tp.Print(true);
}

void HandleNSCreateTable(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    if (parts.size() < 2) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    ::rtidb::client::TableInfo table_info;
    int fd = open(parts[1].c_str(), O_RDONLY);
    if (fd < 0) {
        std::cout << "can not open file " << parts[1] << std::endl;
        return;
    }
    google::protobuf::io::FileInputStream fileInput(fd);
    fileInput.SetCloseOnDelete(true);
    if (!google::protobuf::TextFormat::Parse(&fileInput, &table_info)) {
        std::cout << "table meta file format error" << std::endl;
        return;
    }

    ::rtidb::nameserver::TableInfo ns_table_info;
    ns_table_info.set_name(table_info.name());
    if (table_info.ttl_type() != "kAbsoluteTime" && table_info.ttl_type() != "kLatestTime") {
        printf("ttl type %s is invalid\n", table_info.ttl_type().c_str());
        return;
    }
    if (table_info.table_partition_size() < 1) {
        printf("has not table_partition in table meta file\n");
        return;
    }
    ns_table_info.set_ttl_type(table_info.ttl_type());
    ns_table_info.set_ttl(table_info.ttl());
    ns_table_info.set_seg_cnt(table_info.seg_cnt());
    std::map<uint32_t, std::string> leader_map;
    std::map<uint32_t, std::set<std::string>> follower_map;
    for (int idx = 0; idx < table_info.table_partition_size(); idx++) {
        std::string pid_group = table_info.table_partition(idx).pid_group();
        uint32_t start_index = 0;
        uint32_t end_index = 0;
        if (::rtidb::base::IsNumber(pid_group)) {
            start_index = boost::lexical_cast<uint32_t>(pid_group);
            end_index = start_index;
        } else {
            std::vector<std::string> vec;
            boost::split(vec, pid_group, boost::is_any_of("-"));
            if (vec.size() != 2 || !::rtidb::base::IsNumber(vec[0]) || !::rtidb::base::IsNumber(vec[1])) {
                printf("pid_group[%s] format error.\n", pid_group.c_str());
                return;
            }
            start_index = boost::lexical_cast<uint32_t>(vec[0]);
            end_index = boost::lexical_cast<uint32_t>(vec[1]);

        }
        for (uint32_t pid = start_index; pid <= end_index; pid++) {
            if (table_info.table_partition(idx).is_leader()) {
                if (leader_map.find(pid) != leader_map.end()) {
                    printf("pid %u has two leader\n", pid);
                    return;
                }
                leader_map.insert(std::make_pair(pid, table_info.table_partition(idx).endpoint()));
            } else {
                if (follower_map.find(pid) == follower_map.end()) {
                    follower_map.insert(std::make_pair(pid, std::set<std::string>()));
                }
                if (follower_map[pid].find(table_info.table_partition(idx).endpoint()) != follower_map[pid].end()) {
                    printf("pid %u has same follower on %s\n", pid, table_info.table_partition(idx).endpoint().c_str());
                    return;
                }
                follower_map[pid].insert(table_info.table_partition(idx).endpoint());
            }
        }
    }

    // check follower's leader 
    for (const auto& kv : follower_map) {
        auto iter = leader_map.find(kv.first);
        if (iter == leader_map.end()) {
            printf("pid %u has not leader\n", kv.first);
            return;
        }
        if (kv.second.find(iter->second) != kv.second.end()) {
            printf("pid %u leader and follower at same endpoint %s\n", kv.first, iter->second.c_str());
            return;
        }
    }

    for (const auto& kv : leader_map) {
        ::rtidb::nameserver::TablePartition* table_partition = ns_table_info.add_table_partition();
        table_partition->set_pid(kv.first);
        ::rtidb::nameserver::PartitionMeta* partition_meta = table_partition->add_partition_meta();
        partition_meta->set_endpoint(kv.second);
        partition_meta->set_is_leader(true);
        auto iter = follower_map.find(kv.first);
        if (iter == follower_map.end()) {
            continue;
        }
        // add follower
        for (const auto& endpoint : iter->second) {
            ::rtidb::nameserver::PartitionMeta* partition_meta = table_partition->add_partition_meta();
            partition_meta->set_endpoint(endpoint);
            partition_meta->set_is_leader(false);
        }
    }

    std::set<std::string> type_set;
    type_set.insert("int32");
    type_set.insert("uint32");
    type_set.insert("int64");
    type_set.insert("uint64");
    type_set.insert("float");
    type_set.insert("double");
    type_set.insert("string");
    std::set<std::string> name_set;
    bool has_index = false;
    for (int idx = 0; idx < table_info.column_desc_size(); idx++) {
        if (type_set.find(table_info.column_desc(idx).type()) == type_set.end()) {
            printf("type %s is invalid\n", table_info.column_desc(idx).type().c_str());
            return;
        }
        if (table_info.column_desc(idx).name() == "" || 
                name_set.find(table_info.column_desc(idx).name()) != name_set.end()) {
            printf("check name failed\n");
            return;
        }
        if (table_info.column_desc(idx).add_ts_idx()) {
            has_index = true;
        }
        name_set.insert(table_info.column_desc(idx).name());
        ::rtidb::nameserver::ColumnDesc* column_desc = ns_table_info.add_column_desc();
        column_desc->set_name(table_info.column_desc(idx).name());
        column_desc->set_type(table_info.column_desc(idx).type());
        column_desc->set_add_ts_idx(table_info.column_desc(idx).add_ts_idx());
    }
    if (!has_index && table_info.column_desc_size() > 0) {
        std::cout << "no index" << std::endl;
        return;
    }

    std::string msg;
    if (!client->CreateTable(ns_table_info, msg)) {
        std::cout << "Fail to create table. error msg: " << msg << std::endl;
        return;
    }
    std::cout << "Create table ok" << std::endl;
}

void HandleNSShowOPStatus(const std::vector<std::string>& parts, ::rtidb::client::NsClient* client) {
    std::vector<std::string> row;
    row.push_back("op_id");
    row.push_back("op_type");
    row.push_back("status");
    row.push_back("start_time");
    row.push_back("execute_time");
    row.push_back("end_time");
    row.push_back("cur_task");
    ::baidu::common::TPrinter tp(row.size());
    tp.AddRow(row);
    ::rtidb::nameserver::ShowOPStatusResponse response;
    std::string msg;
    bool ok = client->ShowOPStatus(response, msg);
    if (!ok) {
        std::cout << "Fail to show tablets. error msg: " << msg << std::endl;
        return;
    }
    for (int idx = 0; idx < response.op_status_size(); idx++) { 
        std::vector<std::string> row;
        row.push_back(std::to_string(response.op_status(idx).op_id()));
        row.push_back(response.op_status(idx).op_type());
        row.push_back(response.op_status(idx).status());
        time_t rawtime = (time_t)response.op_status(idx).start_time();
        tm* timeinfo = localtime(&rawtime);
        char buf[20];
        strftime(buf, 20, "%Y%m%d%H%M%S", timeinfo);
        row.push_back(buf);
        if (response.op_status(idx).end_time() != 0) {
            row.push_back(std::to_string(response.op_status(idx).end_time() - response.op_status(idx).start_time()) + "s");
            rawtime = (time_t)response.op_status(idx).end_time();
            timeinfo = localtime(&rawtime);
            buf[0] = '\0';
            strftime(buf, 20, "%Y%m%d%H%M%S", timeinfo);
            row.push_back(buf);
        } else {
            uint64_t cur_time = ::baidu::common::timer::now_time();
            row.push_back(std::to_string(cur_time - response.op_status(idx).start_time()) + "s");
            row.push_back("-");
        }
        row.push_back(response.op_status(idx).task_type());
        tp.AddRow(row);
    }
    tp.Print(true);
}

void HandleClientGet(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 5) {
        std::cout << "Bad get format, eg get tid pid key time" << std::endl;
        return;
    }
    try {
        std::string value;
        uint64_t ts = 0;
        bool ok = client->Get(boost::lexical_cast<uint32_t>(parts[1]),
                              boost::lexical_cast<uint32_t>(parts[2]),
                              parts[3],
                              boost::lexical_cast<uint64_t>(parts[4]),
                              value,
                              ts);
        if (ok) {
            std::cout << "value :" << value << std::endl;
        }else {
            std::cout << "Get failed" << std::endl; 
        }

    
    } catch(std::exception const& e) {
        std::cout << "Invalid args tid and pid should be uint32_t" << std::endl;
    }

}


// the input format like put 1 1 key time value
void HandleClientPut(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 6) {
        std::cout << "Bad put format, eg put tid pid key time value" << std::endl;
        return;
    }
    try {
        bool ok = client->Put(boost::lexical_cast<uint32_t>(parts[1]),
                            boost::lexical_cast<uint32_t>(parts[2]),
                            parts[3],
                            boost::lexical_cast<uint64_t>(parts[4]),
                            parts[5]);
        if (ok) {
            std::cout << "Put ok" << std::endl;
        }else {
            std::cout << "Put failed" << std::endl; 
        }
    } catch(std::exception const& e) {
        std::cout << "Invalid args tid and pid should be uint32_t" << std::endl;
    } 
}

void HandleClientBenPut(std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    uint32_t size = 400;
    try {
        if (parts.size() >= 3) {
            size = boost::lexical_cast<uint32_t>(parts[2]);
        }
        uint32_t times = 10000;
        if (parts.size() >= 4) {
            times = ::boost::lexical_cast<uint32_t>(parts[3]);
        }
        char val[size];
        for (uint32_t i = 0; i < size; i++) {
            val[i] ='0';
        }
        std::string sval(val);
        for (uint32_t i = 0 ; i < times; i++) {
            std::string key = parts[1] + "test" + boost::lexical_cast<std::string>(i);
            for (uint32_t j = 0; j < 1000; j++) {
                client->Put(1, 1, key, j, sval);
            }
            client->ShowTp();
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "put argument error!" << std::endl;
    }
}

// the input format like create name tid pid ttl leader endpoints 
void HandleClientCreateTable(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 6) {
        std::cout << "Bad create format, input like create <name> <tid> <pid> <ttl> <seg_cnt>" << std::endl;
        return;
    }

    try {
        int64_t ttl = 0;
        ::rtidb::api::TTLType type = ::rtidb::api::TTLType::kAbsoluteTime;
        if (parts.size() > 4) {
            std::vector<std::string> vec;
            ::rtidb::base::SplitString(parts[4], ":", &vec);
            if (vec.size() > 1 && vec[0] == "latest") {
                type = ::rtidb::api::TTLType::kLatestTime;
            }
            if (vec.size() > 1 && vec[0] != "latest") {
                std::cout << "invalid ttl type" << std::endl;
                return;
            }
            ttl = boost::lexical_cast<uint64_t>(vec[vec.size() - 1]);
        }
        if (ttl < 0) {
            std::cout << "ttl should be equal or greater than 0" << std::endl;
            return;
        }
        uint32_t seg_cnt = 16;
        if (parts.size() > 5) {
            seg_cnt = boost::lexical_cast<uint32_t>(parts[5]);
        }
        bool is_leader = true;
        if (parts.size() > 6 && parts[6] == "false") {
            is_leader = false;
        }
        std::vector<std::string> endpoints;
        for (size_t i = 7; i < parts.size(); i++) {
            endpoints.push_back(parts[i]);
        }
        bool ok = client->CreateTable(parts[1], 
                                      boost::lexical_cast<uint32_t>(parts[2]),
                                      boost::lexical_cast<uint32_t>(parts[3]), 
                                      (uint64_t)ttl, is_leader, endpoints, type, seg_cnt);
        if (!ok) {
            std::cout << "Fail to create table" << std::endl;
        }else {
            std::cout << "Create table ok" << std::endl;
        }

    } catch(std::exception const& e) {
        std::cout << "Invalid args, tid , pid or ttl should be uint32_t" << std::endl;
    }
}

void HandleClientDropTable(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad drop command, you should input like 'drop tid pid' "<< std::endl;
        return;
    }
    try {
        bool ok = client->DropTable(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]));
        if (ok) {
            std::cout << "Drop table ok" << std::endl;
        }else {
            std::cout << "Fail to drop table" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad drop format" << std::endl;
    }
}

void HandleClientAddReplica(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad addreplica format" << std::endl;
        return;
    }
    try {
        bool ok = client->AddReplica(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]), parts[3]);
        if (ok) {
            std::cout << "AddReplica ok" << std::endl;
        }else {
            std::cout << "Fail to Add Replica" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad addreplica format" << std::endl;
    }
}

void HandleClientDelReplica(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad delreplica format" << std::endl;
        return;
    }
    try {
        bool ok = client->DelReplica(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]), parts[3]);
        if (ok) {
            std::cout << "DelReplica ok" << std::endl;
        }else {
            std::cout << "Fail to Del Replica" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad delreplica format" << std::endl;
    }
}

void HandleClientSetExpire(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    try {
        bool ok = client->SetExpire(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]),
                            parts[3] == "true" ? true : false);
        if (ok) {
            std::cout << "setexpire ok" << std::endl;
        } else {
            std::cout << "Fail to setexpire" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad format" << std::endl;
    }

}

void HandleClientConnectZK(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    bool ok = client->ConnectZK();
    if (ok) {
        std::cout << "connect zk ok" << std::endl;
    } else {
        std::cout << "Fail to connect zk" << std::endl;
    }
}

void HandleClientDisConnectZK(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    bool ok = client->DisConnectZK();
    if (ok) {
        std::cout << "disconnect zk ok" << std::endl;
    } else {
        std::cout << "Fail to disconnect zk" << std::endl;
    }
}

void HandleClientSetTTLClock(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad format" << std::endl;
        return;
    }
    struct tm tm;
    time_t timestamp;
    if (parts[3].length() == 14 && ::rtidb::base::IsNumber(parts[3]) &&
            strptime(parts[3].c_str(), "%Y%m%d%H%M%S", &tm) != NULL) {
        timestamp = mktime(&tm);
    } else {
        printf("time format error (e.g 20171108204001)");
        return;
    }
    try {
        bool ok = client->SetTTLClock(boost::lexical_cast<uint32_t>(parts[1]), 
                                    boost::lexical_cast<uint32_t>(parts[2]), 
                                    timestamp);
        if (ok) {
            std::cout << "setttlclock ok" << std::endl;
        } else {
            std::cout << "Fail to setttlclock" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad format" << std::endl;
    }

}

void AddPrintRow(const ::rtidb::api::TableStatus& table_status, ::baidu::common::TPrinter& tp) {
    std::vector<std::string> row;
    row.push_back(std::to_string(table_status.tid()));
    row.push_back(std::to_string(table_status.pid()));
    row.push_back(std::to_string(table_status.offset()));
    row.push_back(::rtidb::api::TableMode_Name(table_status.mode()));
    row.push_back(::rtidb::api::TableState_Name(table_status.state()));
    if (table_status.is_expire()) {
        row.push_back("true");
    } else {
        row.push_back("false");
    }
    row.push_back(std::to_string(table_status.ttl()) + "min");
    row.push_back(std::to_string(table_status.time_offset()) + "s");
    row.push_back(::rtidb::base::HumanReadableString(table_status.record_byte_size() + table_status.record_idx_byte_size()));
    tp.AddRow(row);
}

void HandleClientGetTableStatus(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    std::vector<std::string> row;
    row.push_back("tid");
    row.push_back("pid");
    row.push_back("offset");
    row.push_back("mode");
    row.push_back("state");
    row.push_back("enable_expire");
    row.push_back("ttl");
    row.push_back("ttl_offset");
    row.push_back("memused");
    ::baidu::common::TPrinter tp(row.size());
    tp.AddRow(row);
    if (parts.size() == 3) {
        ::rtidb::api::TableStatus table_status;
        try {
            if (client->GetTableStatus(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]), table_status) == 0) {
                AddPrintRow(table_status, tp);
                tp.Print(true);
            } else {
                std::cout << "gettablestatus failed" << std::endl;
            }
        } catch (boost::bad_lexical_cast& e) {
            std::cout << "Bad gettablestatus format" << std::endl;

        }
    } else if (parts.size() == 1) {
        ::rtidb::api::GetTableStatusResponse response;
        if (client->GetTableStatus(response) < 0) {
            std::cout << "gettablestatus failed" << std::endl;
            return;
        }
        for (int idx = 0; idx < response.all_table_status_size(); idx++) {
            AddPrintRow(response.all_table_status(idx), tp);
        }
        tp.Print(true);
    } else {
        std::cout << "Bad gettablestatus format" << std::endl;
        return;
    }
}

void HandleClientMakeSnapshot(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad MakeSnapshot format" << std::endl;
        return;
    }
    bool ok = client->MakeSnapshot(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]));
    if (ok) {
        std::cout << "MakeSnapshot ok" << std::endl;
    } else {
        std::cout << "Fail to MakeSnapshot" << std::endl;
    }
}

void HandleClientPauseSnapshot(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad PauseSnapshot format" << std::endl;
        return;
    }
    try {
        bool ok = client->PauseSnapshot(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]));
        if (ok) {
            std::cout << "PauseSnapshot ok" << std::endl;
        }else {
            std::cout << "Fail to PauseSnapshot" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad PauseSnapshot format" << std::endl;
    }
}

void HandleClientRecoverSnapshot(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 3) {
        std::cout << "Bad RecoverSnapshot format" << std::endl;
        return;
    }
    try {
        bool ok = client->RecoverSnapshot(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]));
        if (ok) {
            std::cout << "RecoverSnapshot ok" << std::endl;
        }else {
            std::cout << "Fail to RecoverSnapshot" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad RecoverSnapshot format" << std::endl;
    }
}

void HandleClientSendSnapshot(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad SendSnapshot format" << std::endl;
        return;
    }
    try {
        bool ok = client->SendSnapshot(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]), parts[3]);
        if (ok) {
            std::cout << "SendSnapshot ok" << std::endl;
        }else {
            std::cout << "Fail to SendSnapshot" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad SendSnapshot format" << std::endl;
    }
}

void HandleClientLoadTable(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 6) {
        std::cout << "Bad LoadTable format eg loadtable <name> <tid> <pid> <ttl> <seg_cnt> " << std::endl;
        return;
    }
    try {
        uint64_t ttl = 0;
        if (parts.size() > 4) {
            ttl = boost::lexical_cast<uint64_t>(parts[4]);
        }
        uint32_t seg_cnt = 16;
        if (parts.size() > 5) {
            seg_cnt = boost::lexical_cast<uint32_t>(parts[5]);
        }
        bool is_leader = true;
        if (parts.size() > 6 && parts[6] == "false") {
            is_leader = false;
        }
        std::vector<std::string> endpoints;
        for (size_t i = 7; i < parts.size(); i++) {
            endpoints.push_back(parts[i]);
        }

        bool ok = client->LoadTable(parts[1], boost::lexical_cast<uint32_t>(parts[2]),
                                    boost::lexical_cast<uint32_t>(parts[3]), 
                                    ttl,
                                    is_leader, endpoints, seg_cnt);
        if (ok) {
            std::cout << "LoadTable ok" << std::endl;
        }else {
            std::cout << "Fail to LoadTable" << std::endl;
        }
    } catch (boost::bad_lexical_cast& e) {
        std::cout << "Bad LoadTable format" << std::endl;
    }
}

void HandleClientChangeRole(const std::vector<std::string> parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 4) {
        std::cout << "Bad changerole format" << std::endl;
        return;
    }
     if (parts[3].compare("leader") == 0) {
        try {
            bool ok = client->ChangeRole(boost::lexical_cast<uint32_t>(parts[1]), boost::lexical_cast<uint32_t>(parts[2]), true);
            if (ok) {
                std::cout << "ChangeRole ok" << std::endl;
            } else {
                std::cout << "Fail to Change leader" << std::endl;
            }
        } catch (boost::bad_lexical_cast& e) {
            std::cout << "Bad changerole format" << std::endl;
        }
    } else {
        std::cout << "not support to change follower" << std::endl;
    }
}

// the input format like scan tid pid pk st et
void HandleClientScan(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 6) {
        std::cout << "Bad scan format" << std::endl;
        return;
    }
    try {
        ::rtidb::base::KvIterator* it = client->Scan(boost::lexical_cast<uint32_t>(parts[1]), 
                boost::lexical_cast<uint32_t>(parts[2]),
                parts[3], boost::lexical_cast<uint64_t>(parts[4]), 
                boost::lexical_cast<uint64_t>(parts[5]));
        if (it == NULL) {
            std::cout << "Fail to scan table" << std::endl;
        }else {
            bool print = true;
            if (parts.size() >= 7) {
                if (parts[6] == "false") {
                    print = false;
                }
            }
            std::cout << "#\tTime\tData" << std::endl;
            uint32_t index = 1;
            while (it->Valid()) {
                if (print) {
                    std::cout << index << "\t" << it->GetKey() << "\t" << it->GetValue().ToString() << std::endl;
                } 
                index ++;
                it->Next();
            }
            delete it;
        }

    } catch (std::exception const& e) {
        std::cout<< "Invalid args, tid pid should be uint32_t, st and et should be uint64_t" << std::endl;
    }
}

void HandleClientBenchmarkPut(uint32_t tid, uint32_t pid,
                              uint32_t val_size, uint32_t run_times,
                              uint32_t ns,
        ::rtidb::client::TabletClient* client) {
    char val[val_size];
    for (uint32_t i = 0; i < val_size; i++) {
        val[i] ='0';
    }
    std::string sval(val);
    for (uint32_t i = 0 ; i < run_times; i++) {
        std::string key = boost::lexical_cast<std::string>(ns) + "test" + boost::lexical_cast<std::string>(i);
        for (uint32_t j = 0; j < 4000; j++) {
            client->Put(tid, pid, key, j, sval);
        }
        client->ShowTp();
    }
}

void HandleClientBenchmarkScan(uint32_t tid, uint32_t pid,
        uint32_t run_times, 
        uint32_t ns,
        ::rtidb::client::TabletClient* client) {
    uint64_t st = 999;
    uint64_t et = 0;
    for (uint32_t j = 0; j < run_times; j++) {
        for (uint32_t i = 0; i < 500 * 4; i++) {
            std::string key =boost::lexical_cast<std::string>(ns) + "test" + boost::lexical_cast<std::string>(i);
            ::rtidb::base::KvIterator* it = client->Scan(tid, pid, key, st, et);
            delete it;
        }
        client->ShowTp();
    }
}


void HandleClientBenchmark(::rtidb::client::TabletClient* client) {
    uint32_t size = 40;
    uint32_t times = 10;
    std::cout << "Percentile:Start benchmark put size:40" << std::endl;
    HandleClientBenchmarkPut(1, 1, size, times, 1, client);
    std::cout << "Percentile:Start benchmark put size:80" << std::endl;
    HandleClientBenchmarkPut(1, 1, 80, times, 2, client);
    std::cout << "Percentile:Start benchmark put size:200" << std::endl;
    HandleClientBenchmarkPut(1, 1, 200, times, 3, client);
    std::cout << "Percentile:Start benchmark put ha size:400" << std::endl;
    HandleClientBenchmarkPut(1, 1, 400, times, 4, client);

    std::cout << "Percentile:Start benchmark put with one replica size:40" << std::endl;
    HandleClientBenchmarkPut(2, 1, size, times, 1, client);
    std::cout << "Percentile:Start benchmark put with one replica size:80" << std::endl;
    HandleClientBenchmarkPut(2, 1, 80, times, 2, client);
    std::cout << "Percentile:Start benchmark put with one replica  size:200" << std::endl;
    HandleClientBenchmarkPut(2, 1, 200, times, 3, client);
    std::cout << "Percentile:Start benchmark put with one replica size:400" << std::endl;
    HandleClientBenchmarkPut(2, 1, 400, times, 4, client);

    std::cout << "Percentile:Start benchmark Scan 1000 records key size:40" << std::endl;
    HandleClientBenchmarkScan(1, 1, times, 1, client);
    std::cout << "Percentile:Start benchmark Scan 1000 records key size:80" << std::endl;
    HandleClientBenchmarkScan(1, 1, times, 2, client);
    std::cout << "Percentile:Start benchmark Scan 1000 records key size:200" << std::endl;
    HandleClientBenchmarkScan(1, 1, times, 3, client);
    std::cout << "Percentile:Start benchmark Scan 1000 records key size:400" << std::endl;
    HandleClientBenchmarkScan(1, 1, times, 4, client);
}

void HandleClientSCreateTable(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 6) {
        std::cout << "Bad create format, input like create <name> <tid> <pid> <ttl> <seg_cnt>" << std::endl;
        return;
    }
    try {
        int64_t ttl = 0;
        ::rtidb::api::TTLType type = ::rtidb::api::TTLType::kAbsoluteTime;
        if (parts.size() > 4) {
            std::vector<std::string> vec;
            ::rtidb::base::SplitString(parts[4], ":", &vec);
            if (vec.size() > 1 && vec[0] == "latest") {
                type = ::rtidb::api::TTLType::kLatestTime;
            }
            if (vec.size() > 1 && vec[0] != "latest" ) {
                std::cout << "invalid ttl type " << std::endl;
                return;
            }
            ttl = boost::lexical_cast<int64_t>(vec[vec.size() - 1]);
        }
        if (ttl < 0) {
            std::cout << "invalid ttl which should be equal or greater than 0" << std::endl;
            return;
        }
        uint32_t seg_cnt = 16;
        if (parts.size() > 5) {
            seg_cnt = boost::lexical_cast<uint32_t>(parts[5]);
        }
        bool leader = true;
        if (parts.size() > 6 && parts[6].compare("false") == 0) {
            leader = false;
        }
        std::vector<::rtidb::base::ColumnDesc> columns;
        // check duplicate column
        std::set<std::string> used_column_names;
        for (uint32_t i = 7; i < parts.size(); i++) {
            std::vector<std::string> kv;
            ::rtidb::base::SplitString(parts[i], ":", &kv);
            if (kv.size() < 2) {
                continue;
            }
            if (used_column_names.find(kv[0]) != used_column_names.end()) {
                std::cout << "Duplicated column " << kv[0] << std::endl;
                return;
            }
            used_column_names.insert(kv[0]);
            bool add_ts_idx = false;
            if (kv.size() > 2 && kv[2] == "index") {
                add_ts_idx = true;
            }
            ::rtidb::base::ColType type;
            if (kv[1] == "int32") {
                type = ::rtidb::base::ColType::kInt32;
            }else if (kv[1] == "int64") {
                type = ::rtidb::base::ColType::kInt64;
            }else if (kv[1] == "uint32") {
                type = ::rtidb::base::ColType::kUInt32;
            }else if (kv[1] == "uint64") {
                type = ::rtidb::base::ColType::kUInt64;
            }else if (kv[1] == "float") {
                type = ::rtidb::base::ColType::kFloat;
            }else if (kv[1] == "double") {
                type = ::rtidb::base::ColType::kDouble;
            }else if (kv[1] == "string") {
                type = ::rtidb::base::ColType::kString;
            }else {
                continue;
            }
            ::rtidb::base::ColumnDesc desc;
            desc.add_ts_idx = add_ts_idx;
            desc.type = type;
            desc.name = kv[0];
            columns.push_back(desc);
        }
        bool ok = client->CreateTable(parts[1], 
                                      boost::lexical_cast<uint32_t>(parts[2]),
                                      boost::lexical_cast<uint32_t>(parts[3]), 
                                      (uint64_t)ttl, seg_cnt, columns, type, leader,
                                      std::vector<std::string>());
        if (!ok) {
            std::cout << "Fail to create table" << std::endl;
        }else {
            std::cout << "Create table ok" << std::endl;
        }

    } catch(std::exception const& e) {
        std::cout << "Invalid args " << e.what() << std::endl;
    }
}

void HandleClientShowSchema(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 3) {
        std::cout <<  "Bad show schema format" << std::endl;
        return;
    }
    std::string schema;
    bool ok = client->GetTableSchema(boost::lexical_cast<uint32_t>(parts[1]),
                                    boost::lexical_cast<uint32_t>(parts[2]), schema);
    if(!ok || schema.empty()) {
        std::cout << "No schema for table" << std::endl;
        return;
    }
    std::vector<::rtidb::base::ColumnDesc> raw;
    ::rtidb::base::SchemaCodec codec;
    codec.Decode(schema, raw);
    ::baidu::common::TPrinter tp(4);
    std::vector<std::string> header;
    header.push_back("#");
    header.push_back("name");
    header.push_back("type");
    header.push_back("index");

    tp.AddRow(header);
    for (uint32_t i = 0; i < raw.size(); i++) {
        std::vector<std::string> row;
        row.push_back(boost::lexical_cast<std::string>(i));
        row.push_back(raw[i].name);
        switch (raw[i].type) {
            case ::rtidb::base::ColType::kInt32:
                row.push_back("int32");
                break;
            case ::rtidb::base::ColType::kInt64:
                row.push_back("int64");
                break;
            case ::rtidb::base::ColType::kUInt32:
                row.push_back("uint32");
                break;
            case ::rtidb::base::ColType::kUInt64:
                row.push_back("uint64");
                break;
            case ::rtidb::base::ColType::kDouble:
                row.push_back("double");
                break;
            case ::rtidb::base::ColType::kFloat:
                row.push_back("float");
                break;
            case ::rtidb::base::ColType::kString:
                row.push_back("string");
                break;
            default:
                break;
        }
        if (raw[i].add_ts_idx) {
            row.push_back("yes");
        }else {
            row.push_back("no");
        }
        tp.AddRow(row);
    }
    tp.Print(true);
}

uint32_t GetDimensionIndex(const std::vector<::rtidb::base::ColumnDesc>& columns,
                           const std::string& dname) {
    uint32_t dindex = 0;
    for (uint32_t i = 0; i < columns.size(); i++) {
        if (columns[i].name == dname) {
            return dindex;
        }
        if (columns[i].add_ts_idx) {
            dindex ++;
        }
    }
    return 0;
}

void HandleClientSGet(const std::vector<std::string>& parts, 
                      ::rtidb::client::TabletClient* client){
    try {
        if (parts.size() < 4) {
            std::cout << "Bad get format, eg sget tid pid key [time]" << std::endl;
            return;
        }
        uint64_t time = 0;
        if (parts.size() > 4) {
            time = boost::lexical_cast<uint64_t>(parts[4]);
        }
        std::string schema;
        bool ok = client->GetTableSchema(boost::lexical_cast<uint32_t>(parts[1]),
                                         boost::lexical_cast<uint32_t>(parts[2]), 
                                         schema);
        if(!ok) {
            std::cout << "No schema for table ,please use command get" << std::endl;
            return;
        }
        std::vector<::rtidb::base::ColumnDesc> raw;
        ::rtidb::base::SchemaCodec codec;
        codec.Decode(schema, raw);
        ::baidu::common::TPrinter tp(raw.size() + 2, 128);
        std::vector<std::string> row;
        row.push_back("#");
        row.push_back("ts");
        for (uint32_t i = 0; i < raw.size(); i++) {
            row.push_back(raw[i].name);
        }
        tp.AddRow(row);

        std::string value;
        uint64_t ts = 0;
        ok = client->Get(boost::lexical_cast<uint32_t>(parts[1]),
                              boost::lexical_cast<uint32_t>(parts[2]),
                              parts[3],
                              time,
                              value,
                              ts); 
        if (!ok) {
            std::cout << "Fail to get value!" << std::endl;
            return;
        }
        ShowTableRow(raw, value.c_str(), value.size(), ts, 1, tp);
        tp.Print(true);
    } catch (std::exception const& e) {
        std::cout << "Invalid args" << std::endl;
    }
    
}

void HandleClientSScan(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 7) {
        std::cout << "Bad scan format" << std::endl;
        return;
    }
    try {
        ::rtidb::base::KvIterator* it = client->Scan(boost::lexical_cast<uint32_t>(parts[1]), 
                boost::lexical_cast<uint32_t>(parts[2]),
                parts[3], 
                boost::lexical_cast<uint64_t>(parts[5]), 
                boost::lexical_cast<uint64_t>(parts[6]),
                parts[4]);
        if (it == NULL) {
            std::cout << "Fail to scan table" << std::endl;
        }else {
            std::string schema;
            bool ok = client->GetTableSchema(boost::lexical_cast<uint32_t>(parts[1]),
                                             boost::lexical_cast<uint32_t>(parts[2]), schema);
            if(!ok) {
                std::cout << "No schema for table ,please use command scan" << std::endl;
                return;
            }
            ShowTableRows(schema, it);
            delete it;
        }

    } catch (std::exception const& e) {
        std::cout<< "Invalid args, tid pid should be uint32_t, st and et should be uint64_t" << std::endl;
    }

}

void HandleClientSPut(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    if (parts.size() < 5) {
        std::cout << "Bad put format, eg put tid pid time value" << std::endl;
        return;
    }
    try {
        std::string schema;
        bool ok = client->GetTableSchema(boost::lexical_cast<uint32_t>(parts[1]),
                                         boost::lexical_cast<uint32_t>(parts[2]),
                                         schema);

        if (!ok) {
            std::cout << "Fail to get table schema" << std::endl;
            return;
        }

        if (schema.empty()) {
            std::cout << "No schema for table, please use put command" << std::endl;
            return;
        }
        std::vector<::rtidb::base::ColumnDesc> raw;
        ::rtidb::base::SchemaCodec scodec;
        scodec.Decode(schema, raw);
        std::string buffer;
        uint32_t cnt = parts.size() - 4;
        if (cnt != raw.size()) {
            std::cout << "Input value mismatch schema" << std::endl;
            return;
        }
        ::rtidb::base::FlatArrayCodec codec(&buffer, (uint8_t) cnt);
        std::vector<std::pair<std::string, uint32_t> > dimensions;
        uint32_t idx_cnt = 0;
        for (uint32_t i = 4; i < parts.size(); i++) {
            if (i-4 >= raw.size()) {
                std::cout << "Input mismatch schema" << std::endl;
                return;
            }
            if (raw[i - 4].add_ts_idx) {
                dimensions.push_back(std::make_pair(parts[i], idx_cnt));
                idx_cnt ++;
            }
            bool codec_ok = false;
            if (raw[i - 4].type == ::rtidb::base::ColType::kInt32) {
                codec_ok = codec.Append(boost::lexical_cast<int32_t>(parts[i]));
            }else if (raw[i - 4].type == ::rtidb::base::ColType::kInt64) {
                codec_ok = codec.Append(boost::lexical_cast<int64_t>(parts[i]));
            }else if (raw[i - 4].type == ::rtidb::base::ColType::kUInt32) {
                codec_ok = codec.Append(boost::lexical_cast<uint32_t>(parts[i]));
            }else if (raw[i - 4].type == ::rtidb::base::ColType::kUInt64) {
                codec_ok = codec.Append(boost::lexical_cast<uint64_t>(parts[i]));
            }else if (raw[i - 4].type == ::rtidb::base::ColType::kFloat) {
                codec_ok = codec.Append(boost::lexical_cast<float>(parts[i]));
            }else if (raw[i - 4].type == ::rtidb::base::ColType::kDouble) {
                codec_ok = codec.Append(boost::lexical_cast<double>(parts[i]));
            }else if (raw[i - 4].type == ::rtidb::base::ColType::kString) {
                codec_ok = codec.Append(parts[i]);
            }
            if (!codec_ok) {
                std::cout << "Failed invalid value " << parts[i] << std::endl;
                return;
            }
        }
        codec.Build();
        ok = client->Put(boost::lexical_cast<uint32_t>(parts[1]),
                         boost::lexical_cast<uint32_t>(parts[2]),
                         boost::lexical_cast<uint64_t>(parts[3]),
                         buffer,
                         dimensions);
        if (ok) {
            std::cout << "Put ok" << std::endl;
        }else {
            std::cout << "Put failed" << std::endl; 
        }
    } catch(std::exception const& e) {
        std::cout << e.what() << std::endl;
    } 
}

void HandleClientBenScan(const std::vector<std::string>& parts, ::rtidb::client::TabletClient* client) {
    uint64_t st = 999;
    uint64_t et = 0;
    uint32_t tid = 1;
    uint32_t pid = 1;
    uint32_t times = 10;
    if (parts.size() >= 3) {
        try {
            times = ::boost::lexical_cast<uint32_t>(parts[2]);
        } catch (boost::bad_lexical_cast& e) {
            std::cout << "Bad scan format" << std::endl;
            return;
        }
    }

    for (uint32_t i = 0; i < 10; i++) {
        std::string key = parts[1] + "test" + boost::lexical_cast<std::string>(i);
        ::rtidb::base::KvIterator* it = client->Scan(tid, pid, key, st, et);
        delete it;
    }
    client->ShowTp();
    for (uint32_t j = 0; j < times; j++) {
        for (uint32_t i = 0; i < 500; i++) {
            std::string key = parts[1] + "test" + boost::lexical_cast<std::string>(i);
            ::rtidb::base::KvIterator* it = client->Scan(tid, pid, key, st, et);
            delete it;
        }
        client->ShowTp();
    }
}

void StartClient() {
    //::baidu::common::SetLogLevel(DEBUG);
    std::cout << "Welcome to rtidb with version "<< RTIDB_VERSION_MAJOR
        << "." << RTIDB_VERSION_MINOR << "."<<RTIDB_VERSION_BUG << std::endl;
    ::rtidb::client::TabletClient client(FLAGS_endpoint);
    client.Init();
    while (true) {
        std::cout << ">";
        std::string buffer;
        if (!FLAGS_interactive) {
            buffer = FLAGS_cmd;
        } else {
            std::getline(std::cin, buffer);
            if (buffer.empty()) {
                continue;
            }
        }
        std::vector<std::string> parts;
        ::rtidb::base::SplitString(buffer, " ", &parts);
        if (parts[0] == "put") {
            HandleClientPut(parts, &client);
        } else if (parts[0] == "sput") {
            HandleClientSPut(parts, &client);
        } else if (parts[0] == "create") {
            HandleClientCreateTable(parts, &client);
        } else if (parts[0] == "get") {
            HandleClientGet(parts, &client);
        } else if (parts[0] == "sget") {
            HandleClientSGet(parts, &client);
        }else if (parts[0] == "screate") {
            HandleClientSCreateTable(parts, &client);
        } else if (parts[0] == "scan") {
            HandleClientScan(parts, &client);
        } else if (parts[0] == "sscan") {
            HandleClientSScan(parts, &client);
        } else if (parts[0] == "showschema") {
            HandleClientShowSchema(parts, &client);
        } else if (parts[0] == "benput") {
            HandleClientBenPut(parts, &client);
        } else if (parts[0] == "benscan") {
            HandleClientBenScan(parts, &client);
        } else if (parts[0] == "benchmark") {
            HandleClientBenchmark(&client);
        } else if (parts[0] == "drop") {
            HandleClientDropTable(parts, &client);
        } else if (parts[0] == "addreplica") {
            HandleClientAddReplica(parts, &client);
        } else if (parts[0] == "delreplica") {
            HandleClientDelReplica(parts, &client);
        } else if (parts[0] == "makesnapshot") {
            HandleClientMakeSnapshot(parts, &client);
        } else if (parts[0] == "pausesnapshot") {
            HandleClientPauseSnapshot(parts, &client);
        } else if (parts[0] == "recoversnapshot") {
            HandleClientRecoverSnapshot(parts, &client);
        } else if (parts[0] == "sendsnapshot") {
            HandleClientSendSnapshot(parts, &client);
        } else if (parts[0] == "loadtable") {
            HandleClientLoadTable(parts, &client);
        } else if (parts[0] == "changerole") {
            HandleClientChangeRole(parts, &client);
        } else if (parts[0] == "gettablestatus") {
            HandleClientGetTableStatus(parts, &client);
        } else if (parts[0] == "setexpire") {
            HandleClientSetExpire(parts, &client);
        } else if (parts[0] == "setttlclock") {
            HandleClientSetTTLClock(parts, &client);
        } else if (parts[0] == "connectzk") {
            HandleClientConnectZK(parts, &client);
        } else if (parts[0] == "disconnectzk") {
            HandleClientDisConnectZK(parts, &client);
        } else if (parts[0] == "exit" || parts[0] == "quit") {
            std::cout << "bye" << std::endl;
            return;
        } else {
            std::cout << "unsupported cmd" << std::endl;
        }

        if (!FLAGS_interactive) {
            return;
        }
    }

}

void StartNsClient() {
    
    ::rtidb::client::NsClient client(FLAGS_endpoint);
    client.Init();
    while (true) {
        std::cout << ">";
        std::string buffer;
        if (!FLAGS_interactive) {
            buffer = FLAGS_cmd;
        } else {
            std::getline(std::cin, buffer);
            if (buffer.empty()) {
                continue;
            }
        }
        std::vector<std::string> parts;
        ::rtidb::base::SplitString(buffer, " ", &parts);
        if (parts[0] == "showtablet") {
            HandleNSShowTablet(parts, &client);
        } else if (parts[0] == "showopstatus") {
            HandleNSShowOPStatus(parts, &client);
        } else if (parts[0] == "create") {
            HandleNSCreateTable(parts, &client);
        } else if (parts[0] == "makesnapshot") {
            HandleNSMakeSnapshot(parts, &client);
        } else if (parts[0] == "addreplica") {
            HandleNSAddReplica(parts, &client);
        } else if (parts[0] == "delreplica") {
            HandleNSDelReplica(parts, &client);
        } else if (parts[0] == "drop") {
            HandleNSClientDropTable(parts, &client);
        } else if (parts[0] == "showtable") {
            HandleNSClientShowTable(parts, &client);
        } else if (parts[0] == "confset") {
            HandleNSClientConfSet(parts, &client);
        } else if (parts[0] == "confget") {
            HandleNSClientConfGet(parts, &client);
        } else if (parts[0] == "changeleader") {
            HandleNSClientChangeLeader(parts, &client);
        } else if (parts[0] == "offlineendpoint") {
            HandleNSClientOfflineEndpoint(parts, &client);
        } else if (parts[0] == "recoverendpoint") {
            HandleNSClientRecoverEndpoint(parts, &client);
        } else if (parts[0] == "connectzk") {
            HandleNSClientConnectZK(parts, &client);
        } else if (parts[0] == "disconnectzk") {
            HandleNSClientDisConnectZK(parts, &client);
        } else if (parts[0] == "exit" || parts[0] == "quit") {
            std::cout << "bye" << std::endl;
            return;
        } else {
            std::cout << "unsupported cmd" << std::endl;
        }
        if (!FLAGS_interactive) {
            return;
        }
    }

}

int main(int argc, char* argv[]) {
    ::google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_role == "tablet") {
        StartTablet();
    }else if (FLAGS_role == "client") {
        StartClient();
    }else if (FLAGS_role == "nameserver") {
        StartNameServer();
    }else if (FLAGS_role == "ns_client") {
        StartNsClient();
    }
    return 0;
}
