# Limit Order Book Simulator and Market-Making Backtester

> **Status: work in progress.** This repository currently holds the project plan only. No code or results have been published yet.

## Goal

Build a limit order book from scratch in C++ and test inventory-aware market making against naive quoting.

## Planned scope

- **Matching engine.** C++17 limit order book with price-time priority. It replays [LOBSTER](https://lobsterdata.com) message files and, after every message, checks the book state against the reference order-book file.
- **Quoting strategy.** Avellaneda and Stoikov (2008), "High-frequency trading in a limit order book". The reservation price is $r(s,q,t) = s - q\gamma\sigma^2(T-t)$ and the total quoted spread is $\delta^a + \delta^b = \gamma\sigma^2(T-t) + \frac{2}{\gamma}\ln\left(1 + \frac{\gamma}{k}\right)$, where $q$ is inventory, $\gamma$ is risk aversion, $\sigma$ is volatility and $k$ is the decay rate of order-arrival intensity.
- **Sanity check.** Reproduce the paper's own simulation comparison against symmetric quoting before using real order flow.
- **Stress test.** Inject informed order flow (Hawkes-process bursts) and measure fill rate and short-horizon markouts to quantify adverse selection.
- **Python bindings.** pybind11 interface for research notebooks.

## Data

LOBSTER offers free single-day sample files. Larger experiments will run on simulated order flow calibrated to the sample. No LOBSTER data is stored or redistributed in this repository.

## Disclaimer

Educational project. Not investment advice.
