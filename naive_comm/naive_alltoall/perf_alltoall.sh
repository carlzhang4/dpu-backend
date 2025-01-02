dpu-upmem-dpurte-clang -o dpu_user dpu_user.c 
gcc -O3 -g --std=c99 -o naive_alltoall naive_alltoall.c  `dpu-pkg-config --cflags --libs dpu` -pthread
sudo rm -rf  perf.*
#sudo bash -c 'perf  record -e cpu-clock -g  ./naive_alltoall'
sudo perf record -e cpu-clock --call-graph dwarf ./naive_alltoall
sudo perf  script -i perf.data &> perf.unfold
sudo /home/pimnic/ziyu/DownloadFlameGraph/stackcollapse-perf.pl perf.unfold &> perf.folded
sudo /home/pimnic/ziyu/DownloadFlameGraph/flamegraph.pl perf.folded > perf.svg