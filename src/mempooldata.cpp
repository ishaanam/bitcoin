#include <mempooldata.h>


std::string Buckets::toString() const {
    std::string ret = "{";

    for (const auto& [range, num] : map) {
        if (range.max.has_value()) {
            ret += strprintf("[%i-%i]:%i,", range.min, range.max.value(), num);
        } else {
            ret += strprintf("[%i+]:%i", range.min, num);
        }
    }

    ret += "}";
    return ret;
}

void Buckets::Update(int range_value) {
    if (range_value >= (max - increment)) {
        map[NumRange(max - increment)] = range_value;
    } else {
        int min = (range_value - (range_value % increment));
        map[NumRange(min, min + increment)] += 1;
    }
}
