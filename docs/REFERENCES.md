# References

## Must-read
- WK Selph, *How to Build a Fast Limit Order Book* (2011) — the classic short post.
  Original blog is down; mirrored at https://gist.github.com/halfelf/db1ae032dc34278968f8bf31ee999a25
  ⚠ It gives away a design — read it **after** you've answered 02_DESIGN_QUESTIONS.md, then compare.
- "The limit order book question every trading interview reaches" —
  https://www.techinterview.org/post/3233477310/limit-order-book-matching-engine-interview/
  (also spoils the answer — read after.)

## Books
- Sourav Ghosh, *Building Low Latency Applications with C++* (Packt, 2023) — ch. 6 builds a matching engine.
- Larry Harris, *Trading and Exchanges* — the market-microstructure bible (order types, why priority rules exist).
- Bouchaud et al., *Trades, Quotes and Prices* (Cambridge) — LOB from a quant-research angle.

## Data
- LOBSTER (NASDAQ order-book reconstruction, free sample files on site): https://lobsterdata.com
- NASDAQ TotalView-ITCH sample files (huge; message-by-message): https://emi.nasdaq.com/ITCH/

## Example repos (look for ideas on testing/benchmarking, not to copy)
- https://github.com/jxm35/LimitOrderBook-MatchingEngine
- https://github.com/chronoxor/CppTrader (ITCH handler + benchmarks, production-grade)
- https://github.com/quantoor/TradingEngineSimulator
- https://tuna.is-a.dev/blog/order-book-matching-engine (blog walkthrough)
