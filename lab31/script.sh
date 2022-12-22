#!/bin/bash
for i in {1..510};
do wget http://static.kremlin.ru/media/events/video/ru/video_high/LJmJ5nrjhyCfVNDigS1CHdlmaG15G8cR.mp4 -e use_proxy=on -e http_proxy=127.0.0.1:20001 -O  $i.tmp &
done
