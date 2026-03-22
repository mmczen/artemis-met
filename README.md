# Artemis II MET

Simple C++ mission elapsed time counter for Artemis II.

## Build

```sh
c++ -std=c++17 -O2 -Wall -Wextra -pedantic -o artemis_ii_met artemis_ii_met.cpp
```

## Run

```sh
./artemis_ii_met
```

The default launch timestamp is `2026-04-01T22:24:00Z`.

You can override it with a UTC ISO-8601 timestamp:

```sh
./artemis_ii_met 2026-04-01T22:24:00Z
```
