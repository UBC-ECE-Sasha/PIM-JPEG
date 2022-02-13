# /bin/bash

python train_classify.py -t -d -n output/mdl_dpu_1/ -e 16 -p 2
python train_classify.py -t -d -n output/mdl_dpu_2/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_3/ -n output/mdl_dpu_3/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_4/ -n output/mdl_dpu_4/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_5/ -n output/mdl_dpu_5/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_6/ -n output/mdl_dpu_6/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_7/ -n output/mdl_dpu_7/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_8/ -n output/mdl_dpu_8/ -e 16 -p 2
python train_classify.py -t -m output/mdl_jpeg_9/ -n output/mdl_dpu_9/ -e 16 -p 2
