# Classify a set of images according to a pre-trained model
# The model was trained on ImageNet and the input images for classification come from
# the test set of tiny-imagenet-200
#
# Based on example in: https://learnopencv.com/deep-learning-with-opencvs-dnn-module-a-definitive-guide/
#
# To use, you must install:
# pip3 install opencv-python
# pip3 install matplotlib
#
# Grab the prebuilt model from:
# https://media.githubusercontent.com/media/onnx/models/master/vision/classification/densenet-121/model/densenet-9.tar.gz
#
# And dataset (ImageNet) class file:
# https://code.ihub.org.cn/projects/729/repository/revisions/master/raw/samples/data/dnn/classification_classes_ILSVRC2012.txt
#
# 2022-01-27 J.Nider

import cv2
import numpy as np

# read the ImageNet class names
with open('../../datasets/classification_classes_ILSVRC2012.txt', 'r') as f:
   image_net_names = f.read().split('\n')
# final class names (just the second word of the many ImageNet names for one image)
class_names = [name.split(',')[0] for name in image_net_names]

# load the neural network model
model = cv2.dnn.readNetFromONNX(onnxFile='../../datasets/densenet121/model.onnx')

# load the image from disk
image = cv2.imread('image.jpg')
# create blob from image
blob = cv2.dnn.blobFromImage(image=image, scalefactor=0.01, size=(224, 224), mean=(104, 117, 123))

# set the input blob for the neural network
model.setInput(blob)
# forward pass image blob through the model
outputs = model.forward()

final_outputs = outputs[0]
# make all the outputs 1D
final_outputs = final_outputs.reshape(1000, 1)
# get the class label
label_id = np.argmax(final_outputs)
# convert the output scores to softmax probabilities
probs = np.exp(final_outputs) / np.sum(np.exp(final_outputs))
# get the final highest probability
final_prob = np.max(probs) * 100.
# map the max confidence to the class label names
out_name = class_names[label_id]
out_text = f"{out_name}, {final_prob:.3f}"
# put the class name text on top of the image
cv2.putText(image, out_text, (25, 50), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
cv2.imshow('Image', image)
cv2.waitKey(0)
