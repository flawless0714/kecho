reset
set xlabel 'Concurrent threads'
set ylabel 'time (us)'
set title 'Kecho concurrent performance'
set term png enhanced font 'Verdana,10'
set output 'perf_measure.png'
set key left

plot [0:1000][0:100000] \
'kecho_perf.txt' using 1:2 with points title 'Avg. time elapsed'
