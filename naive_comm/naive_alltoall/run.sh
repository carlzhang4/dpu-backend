dpu-upmem-dpurte-clang -o dpu_user dpu_user.c 
gcc -O3 --std=c99 -o naive_alltoall naive_alltoall.c  -g `dpu-pkg-config --cflags --libs dpu` -pthread
./naive_alltoall 