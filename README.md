Stream video from one PI to another PI

PI with a camera:
raspivid --bitrate 3500000 --profile high --level 4.2 -vf -hf -n -l -o tcp://0.0.0.0:5001 -w 1920 -h 1080 -fps 0 --intra 0 -ss 1000000 -m raw_tcp

PI with a monitor:
hello_video_active.bin -h 0.0.0.0 -p 12345 -l
