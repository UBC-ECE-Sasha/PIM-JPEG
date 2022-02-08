import os
import pandas as pd
from torchvision.io import read_image
from torch.utils.data import Dataset

class ValDataset(Dataset):
    """
    This class loads a tinyimagenet validation set with its classnames in a txt file
    """
    def __init__(self, img_dir, annotations_file, transform=None, target_transform=None, is_valid_file=None, class_to_idx=None):
        self.img_labels = pd.read_csv(annotations_file, sep='\t', header=None)
        self.img_dir = img_dir
        self.transform = transform
        self.target_transform = target_transform

        if is_valid_file != None:
            v = self.img_labels.iloc[:, 0].apply(is_valid_file)
            self.img_labels = self.img_labels[v]
        self.img_labels.reset_index(drop=True, inplace=True)

        self.class_to_idx = class_to_idx
        if self.class_to_idx != None:
            class_labels = self.img_labels.iloc[:, 1].map(self.class_to_idx)
            self.img_labels.iloc[:, 1] = class_labels

    def __len__(self):
        return len(self.img_labels)

    def __getitem__(self, idx):
        img_path = os.path.join(self.img_dir, self.img_labels.iloc[idx, 0])
        image = read_image(img_path)
        label = self.img_labels.iloc[idx, 1]
        if self.transform:
            image = self.transform(image)
        if self.target_transform:
            label = self.target_transform(label)
        return image, label