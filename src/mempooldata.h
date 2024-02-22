#ifndef BITCOIN_MEMPOOLDATA_H
#define BITCOIN_MEMPOOLDATA_H

#include <map>
#include <optional>
#include <string>

#include <tinyformat.h>

struct NumRange {
    int min; // inclusive
    std::optional<int> max; // exclusive

    NumRange(int min, std::optional<int> max = std::nullopt)
      : min(min),
        max(max) {}

    bool operator<(const NumRange& other) const {
        return min < other.min;
    }
};

struct Buckets {
private:
    std::map<NumRange, int> map;
    int increment;
    int num_buckets;
    int max;
public:
    Buckets(int min, int max, int num_buckets) 
      : num_buckets(num_buckets),
        max(max)
    {
        increment = (max - min) / num_buckets;
        int i = 1;
        for ( ; i < num_buckets; i++) {
            map[NumRange(min + increment * (i - 1), min + increment * (i))] = 0;
        }
        map[NumRange(max - increment)] = 0;
    }

    std::string toString() const;

    void Update(int range_value);
};

struct MempoolData {
    int num_txs{0};
    int collect_data{false};

    Buckets feerate_buckets{0, 600, 30};
    // Buckets size_buckets{};

    void AddTx(int range_value) {
        feerate_buckets.Update(range_value);
    }
};

#endif // BITCOIN_MEMPOOLDATA_H
