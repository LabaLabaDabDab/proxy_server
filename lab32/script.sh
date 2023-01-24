#!/bin/bash
for i in {1..5};
do wget http://static.kremlin.ru/media/events/video/ru/video_high/LJmJ5nrjhyCfVNDigS1CHdlmaG15G8cR.mp4 --timeout=3600 -e use_proxy=on -e http_proxy=127.0.0.1:20000 -O $i.tmp & done
done
