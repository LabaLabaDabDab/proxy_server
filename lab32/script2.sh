#!/bin/bash
for i in {1..510};
do wget http://www.ccfit.nsu.ru/~rzheutskiy/test_files/500mb.dat --limit-rate=500 -e use_proxy=on -e http_proxy=127.0.0.1:20004 -O /dev/null && date &
done
