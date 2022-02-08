from inspect import getsource
from os.path import basename, splitext, isfile, join
from os import walk
from ValDataset import ValDataset
from PIL import Image
import torch
import torchvision

def main():
    # set function to exclude L images
    exclude_greyscale(["../tiny-imagenet-200"])
    exclude = set()
    with open("exclude.txt", "r") as f:
        for line in f.readlines():
            exclude.add(line)
    valid = lambda x: image_name(x) not in exclude

    model = torch.hub.load('pytorch/vision:v0.10.0', 'resnet18', pretrained=False)

    training_jpeg = torchvision.datasets.ImageFolder("../tiny-imagenet-200/train", is_valid_file=valid)
    training_dpu = torchvision.datasets.ImageFolder("../tiny-imagenet-200-dpu/train", is_valid_file=valid)
    val_jpeg = ValDataset("../tiny-imagenet-200/val/images", "../tiny-imagenet-200/val/val_annotations.txt",
        is_valid_file=valid, class_to_idx=training_jpeg.class_to_idx)
    val_dpu = ValDataset("../tiny-imagenet-200-dpu/val/images", "../tiny-imagenet-200-dpu/val/val_annotations.txt",
        is_valid_file=valid)

    print(val_jpeg.__getitem__(1))

def image_name(f):
    f = splitext(basename(f))[0]
    if f.endswith("dpu"):
        f = f[:-4]
    return f
    
def exclude_greyscale(paths, overwrite=False):
    if not isfile("exclude.txt") or overwrite:
        lines = []
        for path in paths:
            for root, dirs, files in walk(path):
                for file in files:
                    if splitext(file)[1].lower() in [".jpeg", ".jpg", ".png", ".bmp"]:
                        with Image.open(join(root, file)) as im:
                            if im.mode != "RGB":
                                lines.append(image_name(file)+"\n")
                                    
        with open("exclude.txt", "w") as f:
            lines[-1] = lines[-1].rstrip()
            f.writelines(lines)
            f.close()


if __name__ == '__main__':
    main()