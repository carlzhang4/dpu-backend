This repository contains the dpu-backend and collective communication code for naive solution and PID-Comm


### How To Make

assume you are in the root directory 
```bash
mkdir build
cd build
cmake ..
make -j
# if you want to use PID dpu code, make below codes
cd pidcomm_lib
make
```

### Run Benchmarks
See [RUN.md](./benchmarks/RUN.md) for run our benchmarks

### Run bf_checksum
<<<<<<< HEAD
Be sure to comment the corresponding two lines in "ci_start_thread_rank()" function of ufi_runner.c