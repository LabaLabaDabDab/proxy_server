#!/bin/bash
for i in {1..510};
do wget http://84.237.52.20/~rzheutskiy/test_files/500mb.dat -e use_proxy=on -e http_proxy=127.0.0.1:20000 -O /dev/null && date &
done
