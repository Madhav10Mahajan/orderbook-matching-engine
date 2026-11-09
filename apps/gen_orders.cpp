// Writes a synthetic order-flow CSV for `replay`.
// Usage: gen_orders <count> <out.csv> [seed=42] [decimals=2]
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "csv.hpp"
#include "order_flow.hpp"

using namespace ob;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gen_orders <count> <out.csv> [seed] [decimals]\n");
        return 2;
    }
    const long count = std::atol(argv[1]);
    const std::uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 42;
    const int decimals = argc > 4 ? std::atoi(argv[4]) : 2;

    std::ofstream out(argv[2]);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    OrderFlowGenerator gen(seed);
    out << "type,id,side,price,qty,tif\n";
    for (long i = 0; i < count; ++i) out << to_csv(gen.next(), decimals) << '\n';
    std::printf("Wrote %ld messages to %s\n", count, argv[2]);
    return 0;
}
