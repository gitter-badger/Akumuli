#include <iostream>
#include <cassert>
#include <random>
#include <tuple>
#include <map>
#include <vector>
#include <algorithm>
#include <memory>

#include <boost/timer.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <apr_mmap.h>
#include <apr_general.h>

#include "akumuli.h"

using namespace std;

const int DB_SIZE = 8;
const int NUM_ITERATIONS = 100*1000*1000;
const int CHUNK_SIZE = 5000;

const char* DB_NAME = "test";
const char* DB_PATH = "./test";
const char* DB_META_FILE = "./test/test.akumuli";

void delete_storage() {
    boost::filesystem::remove_all(DB_PATH);
}

bool query_database_forward(aku_Database* db, aku_TimeStamp begin, aku_TimeStamp end, uint64_t& counter, boost::timer& timer, uint64_t mod) {
    const unsigned int NUM_ELEMENTS = 1000;
    aku_ParamId params[] = {1};
    aku_SelectQuery* query = aku_make_select_query( begin
                                                  , end
                                                  , 42
                                                  , params);
    aku_Cursor* cursor = aku_select(db, query);
    aku_TimeStamp current_time = begin;
    size_t cursor_ix = 0;
    while(!aku_cursor_is_done(cursor)) {
        int err = AKU_SUCCESS;
        if (aku_cursor_is_error(cursor, &err)) {
            std::cout << aku_error_message(err) << std::endl;
            return false;
        }
        aku_TimeStamp timestamps[NUM_ELEMENTS];
        aku_ParamId paramids[NUM_ELEMENTS];
        aku_PData pointers[NUM_ELEMENTS];
        uint32_t lengths[NUM_ELEMENTS];
        int n_entries = aku_cursor_read_columns(cursor, timestamps, paramids, pointers, lengths, NUM_ELEMENTS);
        for (int i = 0; i < n_entries; i++) {
            if (timestamps[i] != current_time) {
                std::cout << "Error at " << cursor_ix << " expected ts " << current_time << " acutal ts " << timestamps[i]  << std::endl;
                return false;
            }
            if (paramids[i] != 42) {
                std::cout << "Error at " << cursor_ix << " expected id 42 acutal id " << paramids[i]  << std::endl;
                return false;
            }
            uint64_t const* pvalue = (uint64_t const*)pointers[i];
            if (*pvalue != current_time + 2) {
                std::cout << "Error at " << cursor_ix << " expected value " << (current_time+2) << " acutal value " << *pvalue  << std::endl;
                return false;
            }
            current_time++;
            counter++;
            if (counter % mod == 0) {
                std::cout << counter << " " << timer.elapsed() << "s" << std::endl;
                timer.restart();
            }
            cursor_ix++;
        }
    }
    aku_close_cursor(cursor);
    if (cursor_ix > 1000) {
        std::cout << "cursor_ix = " << cursor_ix << std::endl;
    }
    return true;
}

void print_storage_stats(aku_StorageStats& ss) {
    std::cout << ss.n_entries << " elenents in" << std::endl
              << ss.n_volumes << " volumes with" << std::endl
              << ss.used_space << " bytes used and" << std::endl
              << ss.free_space << " bytes free" << std::endl;
}

void print_search_stats(aku_SearchStats& ss) {
    std::cout << "Interpolation search" << std::endl;
    std::cout << ss.istats.n_matches << " matches" << std::endl
              << ss.istats.n_times << " times" << std::endl
              << ss.istats.n_steps << " steps" << std::endl
              << ss.istats.n_overshoots << " overshoots" << std::endl
              << ss.istats.n_undershoots << " undershoots" << std::endl
              << ss.istats.n_reduced_to_one_page << "  reduced to page" << std::endl

              << ss.istats.n_page_in_core_checks << "  page_in_core checks" << std::endl
              << ss.istats.n_page_in_core_errors << "  page_in_core errors" << std::endl
              << ss.istats.n_pages_in_core_found  << "  page_in_core success" << std::endl
              << ss.istats.n_pages_in_core_miss  << "  page_in_core miss" << std::endl;

    std::cout << "Binary search" << std::endl;
    std::cout << ss.bstats.n_steps << " steps" << std::endl
              << ss.bstats.n_times << " times" << std::endl;

    std::cout << "Scan" << std::endl;
    std::cout << ss.scan.bwd_bytes << " bytes read in backward direction" << std::endl
              << ss.scan.fwd_bytes << " bytes read in forward direction" << std::endl;
}

enum Mode {
    NONE,
    CREATE,
    DELETE,
    READ
};

Mode read_cmd(int cnt, const char** args) {
    if (cnt < 2) {
        return NONE;
    }
    if (std::string(args[1]) == "create") {
        return CREATE;
    }
    if (std::string(args[1]) == "read") {
        return READ;
    }
    if (std::string(args[1]) == "delete") {
        return DELETE;
    }
    std::cout << "Invalid command line" << std::endl;
    std::terminate();
}

int main(int cnt, const char** args)
{
    Mode mode = read_cmd(cnt, args);

    aku_initialize();

    if (mode == DELETE) {
        delete_storage();
        std::cout << "storage deleted" << std::endl;
        return 0;
    }

    if (mode != READ) {
        // Cleanup
        delete_storage();

        // Create database
        apr_status_t result = aku_create_database(DB_NAME, DB_PATH, DB_PATH, DB_SIZE, nullptr, nullptr, nullptr, nullptr);
        if (result != APR_SUCCESS) {
            std::cout << "Error in new_storage" << std::endl;
            return (int)result;
        }
    }

    aku_FineTuneParams params;
    params.debug_mode = 0;
    params.max_late_write = 10000;
    auto db = aku_open_database(DB_META_FILE, params);
    boost::timer timer;

    if (mode != READ) {
        uint64_t busy_count = 0;
        // Fill in data
        for(uint64_t i = 0; i < NUM_ITERATIONS; i++) {
            uint64_t k = i + 2;
            aku_MemRange memr;
            memr.address = (void*)&k;
            memr.length = sizeof(k);
            aku_Status status = aku_write(db, 42, i, memr);
            if (status == AKU_EBUSY) {
                status = aku_write(db, 42, i, memr);
                busy_count++;
                if (status != AKU_SUCCESS) {
                    std::cout << "add error at " << i << std::endl;
                    return 1;
                }
            }
            if (i % 1000000 == 0) {
                std::cout << i << " " << timer.elapsed() << "s" << std::endl;
                timer.restart();
            }
        }
        std::cout << "!busy count = " << busy_count << std::endl;
    }

    aku_StorageStats storage_stats;
    aku_global_storage_stats(db, &storage_stats);
    print_storage_stats(storage_stats);

    if (mode != CREATE) {
        // Search
        std::cout << "Sequential access" << std::endl;
        aku_SearchStats search_stats;
        uint64_t counter = 0;

        timer.restart();
        if (!query_database_forward( db
                           , std::numeric_limits<aku_TimeStamp>::min()
                           , std::numeric_limits<aku_TimeStamp>::max()
                           , counter
                           , timer
                           , 1000000))
        {
            return 2;
        }

        aku_global_search_stats(&search_stats, true);
        print_search_stats(search_stats);

        // Random access
        std::cout << "Prepare test data" << std::endl;
        std::vector<std::pair<aku_TimeStamp, aku_TimeStamp>> ranges;
        for (aku_TimeStamp i = 1u; i < (aku_TimeStamp)NUM_ITERATIONS/CHUNK_SIZE; i++) {
            std::vector<aku_TimeStamp> range;
            aku_TimeStamp j = (i - 1)*CHUNK_SIZE;
            std::generate_n(std::back_inserter(range), CHUNK_SIZE, [&j]() {return j++;});
            std::random_shuffle(range.begin(), range.end());
            int count = 5;
            for (auto k: range) {
                ranges.push_back(std::make_pair(k, k+1));
                if (!count--) {
                    break;
                }
            }
        }

        std::random_shuffle(ranges.begin(), ranges.end());

        std::cout << "Random access" << std::endl;
        counter = 0;
        timer.restart();
        for(auto range: ranges) {
            if (!query_database_forward(db, range.first, range.second, counter, timer, 10000)) {
                return 3;
            }
        }
        aku_global_search_stats(&search_stats, true);
        print_search_stats(search_stats);
    }

    aku_close_database(db);

    if (mode == NONE) {
        delete_storage();
    }
    return 0;
}

