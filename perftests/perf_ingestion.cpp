#include <iostream>
#include <cassert>
#include <random>
#include <tuple>
#include <map>
#include <vector>
#include <algorithm>
#include <memory>

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <apr_mmap.h>
#include <apr_general.h>
#include <sys/time.h>

#include "akumuli.h"

using namespace std;

int DB_SIZE = 2;
uint64_t NUM_ITERATIONS = 10*1000*1000;
int CHUNK_SIZE = 5000;

const char* DB_NAME = "test";
const char* DB_PATH = "test";
const char* DB_META_FILE = "test/test.akumuli";

class Timer
{
public:
    Timer() { gettimeofday(&_start_time, nullptr); }
    void   restart() { gettimeofday(&_start_time, nullptr); }
    double elapsed() const {
        timeval curr;
        gettimeofday(&curr, nullptr);
        return double(curr.tv_sec - _start_time.tv_sec) +
               double(curr.tv_usec - _start_time.tv_usec)/1000000.0;
    }
private:
    timeval _start_time;
};

int format_timestamp(uint64_t ts, char* buffer) {
    auto fractional = static_cast<int>(ts %  1000000000);  // up to 9 decimal digits
    auto seconds = static_cast<int>(ts / 1000000000);      // two seconds digits
    return sprintf(buffer, "20150102T0304%02d.%09d", seconds, fractional);
}

std::string ts2str(uint64_t ts) {
    char buffer[0x100];
    auto len = format_timestamp(ts, buffer);
    return std::string(buffer, buffer+len);
}

std::string build_query(uint64_t begin, uint64_t end) {
    std::stringstream str;
    str << R"({ "sample": "all", )";
    str << R"("range": { "from": ")" << ts2str(begin)
        << R"(", "to": ")" << ts2str(end)
        << R"("}})";
    return str.str();
}

void delete_storage() {
    aku_remove_database(DB_META_FILE, &aku_console_logger);
}

bool query_database_forward(aku_Database* db, aku_Timestamp begin, aku_Timestamp end, uint64_t& counter, Timer& timer, uint64_t mod) {
    const aku_Timestamp EPOCH = 1420167840000000000;
    const unsigned int NUM_ELEMENTS = 1000;
    std::string query = build_query(begin, end);
    aku_Cursor* cursor = aku_query(db, query.c_str());
    aku_Timestamp current_time = EPOCH + begin;
    size_t cursor_ix = 0;
    while(!aku_cursor_is_done(cursor)) {
        int err = AKU_SUCCESS;
        if (aku_cursor_is_error(cursor, &err)) {
            std::cout << aku_error_message(err) << std::endl;
            return false;
        }
        aku_Sample samples[NUM_ELEMENTS];
        int n_entries = aku_cursor_read(cursor, samples, NUM_ELEMENTS);
        for (int i = 0; i < n_entries; i++) {

            if (samples[i].timestamp != current_time) {
                std::cout << "Error at " << cursor_ix << " expected ts " << current_time << " acutal ts " << samples[i].timestamp  << std::endl;
                current_time = samples[i].timestamp;
            } else {
                double dvalue = samples[i].payload.value.float64;
                double dexpected = (current_time - EPOCH) + 1;
                if (dvalue - dexpected > 0.000001) {
                    std::cout << "Error at " << cursor_ix << " expected value " << dexpected << " acutal value " << dvalue  << std::endl;
                    return false;
                }
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
    aku_cursor_close(cursor);
    auto last_ts = EPOCH + end + 1;
    if (current_time != last_ts) {
        std::cout << "some values lost (1), actual timestamp: " << current_time << ", expected timestamp: " << last_ts << std::endl;
        throw std::runtime_error("values lost");
    }
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
    if (cnt == 4) {
        DB_SIZE        = boost::lexical_cast<int>(args[2]);
        NUM_ITERATIONS = boost::lexical_cast<int>(args[3]);

        if (NUM_ITERATIONS >= 10000000000) {
            std::cout << "NUM_ITERATIONS set too large" << std::endl;
            throw std::runtime_error("Bad command line parameters");
        }
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
    throw std::runtime_error("Bad command line parameters");
}

void logger_(int level, const char * msg) {
    if (level == AKU_LOG_ERROR) {
        //aku_console_logger(level, msg);
    }
}

int main(int cnt, const char** args)
{
    Mode mode = read_cmd(cnt, args);

    aku_initialize(nullptr);

    if (mode == DELETE) {
        delete_storage();
        std::cout << "storage deleted" << std::endl;
        return 0;
    }

    if (mode != READ) {
        // Cleanup
        delete_storage();

        // Create database
        uint32_t threshold  =   1000;
        uint64_t windowsize = 100000;
        uint64_t cachesize =  10*1024*1024;  // 10Mb
        apr_status_t result = aku_create_database(DB_NAME, DB_PATH, DB_PATH, DB_SIZE,
                                                  threshold, windowsize, cachesize, &logger_);
        if (result != APR_SUCCESS) {
            std::cout << "Error in new_storage" << std::endl;
            return (int)result;
        }
    }

    aku_FineTuneParams params = {};
    params.debug_mode = 0;
    params.durability = AKU_MAX_WRITE_SPEED;
    params.enable_huge_tlb = 0;
    auto db = aku_open_database(DB_META_FILE, params);
    Timer timer;

    if (mode != READ) {
        uint64_t busy_count = 0;
        // Fill in data
        for(uint64_t i = 0; i < NUM_ITERATIONS; i++) {
            aku_Sample sample;
            char buffer[100];

            // =series=
            int nchars = sprintf(buffer, "cpu key=%d", ((int)i + 1) & 0xffff);
            aku_series_to_param_id(db, buffer, buffer + nchars, &sample);

            // =timestamp=
            nchars = format_timestamp(i, buffer);
            aku_parse_timestamp(buffer, &sample);

            // =payload=
            sample.payload.type = aku_PData::FLOAT;
            sample.payload.value.float64 = i + 0.1;

            aku_Status status = aku_write(db, &sample);

            while (status == AKU_EBUSY) {
                status = aku_write(db, &sample);
                busy_count++;
            }
            if (i % 1000000 == 0) {
                std::cout << i << " " << timer.elapsed() << "s" << std::endl;
                timer.restart();
            }
        }
        std::cout << "!busy count = " << busy_count << std::endl;
    }

    aku_StorageStats storage_stats = {0};
    aku_global_storage_stats(db, &storage_stats);
    print_storage_stats(storage_stats);

    if (mode != CREATE) {
        // Search
        std::cout << "Sequential access" << std::endl;
        aku_SearchStats search_stats = {0};
        uint64_t counter = 0;

        timer.restart();

        if (!query_database_forward(db, std::numeric_limits<aku_Timestamp>::min(),
                                    NUM_ITERATIONS-1,
                                    counter,
                                    timer,
                                    1000000))
        {
            return 2;
        }

        aku_global_search_stats(&search_stats, true);
        print_search_stats(search_stats);

        // Random access
        std::cout << "Prepare test data" << std::endl;
        std::vector<std::pair<aku_Timestamp, aku_Timestamp>> ranges;
        for (aku_Timestamp i = 1u; i < (aku_Timestamp)NUM_ITERATIONS/CHUNK_SIZE; i++) {
            aku_Timestamp j = (i - 1)*CHUNK_SIZE;
            int count = 5;
            for (int d = 0; d < count; d++) {
                int r = std::rand() % CHUNK_SIZE;
                int k = j + r;
                ranges.push_back(std::make_pair(k, k+1));
            }
        }

        std::random_shuffle(ranges.begin(), ranges.end());

        std::cout << "Random access" << std::endl;
        counter = 0;
        timer.restart();
        for(auto range: ranges) {
            if (!query_database_forward(db, range.first, range.second, counter, timer, 1000)) {
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

