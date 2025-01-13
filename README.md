This repository contains the collective communication code for naive solution and PID-Comm

### Interface

- Naive collective communication interfaces are   defined in `/naive_comm/communication/CommOps.h` and `/naive_comm/naive_alltoall/naive_alltoall.c`
- PID collective communication interfaces are   defined in `/home/pimnic/ziyu/baseline/dpu-backend/api/src/api/dpu_memory.c`



### How To Run
our sample code are in the /example directory and naive_comm/communication directory 
- test.c (in /example)
    test PID collective communication interfaces
- DPU_CPU_Bandwidth.c (in /example)
    test DPU_CPU_Bandwidth using `dpu_push_xfer`
- test.c (in /naive_comm/communication)
    test naive collective communication interfaces
- naive_alltoall.c (in /naive_comm/naive_alltoall)
    test naive collective communication interfaces

assume you are in the root directory 
```bash
mkdir build
cd build
cmake ..
make -j
```

to run these code. enter /build directory and run them
```bash
./example/DPU_CPU_Bandwidth
./example/test_PID
./naive_comm/naive_alltoall/naive_alltoall
./naive_comm/communication/communication_test
```

