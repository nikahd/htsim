# msft-htsim

This repository contains the source code for the Microsoft internal fork of the htsim discrete event simulator.

## Getting Started

htsim is a high-performance discrete event simulator, inspired by ns2 but much faster. It is primarily intended to examine transport protocol behavior. htsim is written in C++ and has no major dependencies.

## Building the Project

Before building, ensure you have cmake (version 3.16 or later) and a C++ compiler that supports C++17 installed. To build the project on a Linux machine, run the following commands:

```bash
cmake -S sim -B build # Configure the cmake project
cmake --build build -j$(nproc) # Build the project
```

To test the project after building, run the following command:

```bash
cd build && ctest --output-on-failure -j$(nproc) # Run the tests
```

## Running a Simulation

The main executable for htsim is `sim/datacenter/main_uec.cpp`. It contains the logic for setting up and running a simulation based on the defined configuration.

Most configuration parameters have default values, which can be overridden using the command line. Example configurations include the transport algorithm to use, type of queues in switches to define switch queueing behavior, etc.

Setting up the simulation involves reading a topology file and setting up the network, reading a workload file and setting up the traffic, and setting up the congestion control algorithm.

For example, after successfully building the project, to start a simple simulation with the default configuration parameters and with just a single 2MB flow between two nodes, run the following command from the root of the repository:

```bash
./build/htsim_uec -tm ./scripts/connection_matrices_old/one.cm
```

## Generating Documentation

To generate the documentation for the project, ensure you have Doxygen installed. Then run the following command from the root of the repository:

```bash
doxygen -s Doxyfile
```

The generated documentation will be placed in the `docs/html` directory. Open `docs/html/index.html` in a web browser to view the documentation.

## Submitting Code Changes

Submitting code changes to the main branch should go through a pull request. A pull request should be created from a feature branch to the main branch. The pull request should be reviewed by at least one other team member and all conversations on the PR be resolved before merging.

The pull request to the main branch will also trigger a CI/CD pipeline using Github Actions that will build the project, run the tests, and check for code style issues. The pull request will not be merged if the pipeline fails.

Before submitting a pull request:

1. Ensure that the code is formatted using clang-format. To format the code, run the following command from the root of the repository:

```bash
find ./sim -regex '.*\.\(cpp\|h\|hpp\)$' -exec clang-format -i --style=file {} +
```

2. Ensure you have written unit tests for the code changes you are submitting *(this is an aspirational goal for now while we are still developing a framework for testing).*

> See `./sim/*_test.cpp` for examples of how to write unit tests.
