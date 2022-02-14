""" Script to train resnet18 on tiny-imagenet-18 with images processed regularly and on the DPU """
from argparse import ArgumentParser
from os.path import basename, splitext, isfile, join, isdir
from os import walk, mkdir
import statistics
from ValDataset import ValDataset
from PIL import Image
import torch
import torch.optim as optim
import torch.nn as nn
import torch.nn.functional as F
import torchvision
from torchvision import transforms

def main():
    args = commandArgs()
    
    device = torch.device('cuda:0' if torch.cuda.is_available() else 'cpu')
    print(device)

    # set function to exclude L images
    exclude_greyscale(["../tiny-imagenet-200"])
    exclude = set()
    with open("exclude.txt", "r") as f:
        for line in f.readlines():
            exclude.add(line.strip())
    valid_jpeg = lambda x: (image_name(x) not in exclude) and x.endswith(".JPEG")
    valid_dpu = lambda x: (image_name(x) not in exclude) and x.endswith(".bmp")

    transform_train = transforms.Compose([
    transforms.ToTensor()
    ])
    transform_val = None

    training_jpeg = torchvision.datasets.ImageFolder("../tiny-imagenet-200/train",
        is_valid_file=valid_jpeg, transform=transform_train)
    training_dpu = torchvision.datasets.ImageFolder("../tiny-imagenet-200-dpu/train",
        is_valid_file=valid_dpu, transform=transform_train)
    val_jpeg = ValDataset("../tiny-imagenet-200/val/images", "../tiny-imagenet-200/val/val_annotations.txt",
        is_valid_file=valid_jpeg, transform=transform_val, class_to_idx=training_jpeg.class_to_idx)
    val_dpu = ValDataset("../tiny-imagenet-200-dpu/val/images", "../tiny-imagenet-200-dpu/val/val_annotations.txt",
        is_valid_file=valid_dpu, transform=transform_val, class_to_idx=training_dpu.class_to_idx)

    if (not args.dpuModelOnly) and (not args.evalOnly):
        model = torch.hub.load('pytorch/vision:v0.10.0', 'resnet18', pretrained=False, num_classes=200)
        train_model(device, model, training_jpeg, val_jpeg, savepath=args.jpegModelOutput)
    if (not args.jpegModelOnly) and (not args.evalOnly):
        model = torch.hub.load('pytorch/vision:v0.10.0', 'resnet18', pretrained=False, num_classes=200)
        train_model(device, model, training_dpu, val_dpu, savepath=args.dpuModelOutput)
    if (not args.dpuModelOnly) and (not args.trainOnly):
        model = torch.hub.load('pytorch/vision:v0.10.0', 'resnet18', pretrained=False, num_classes=200)
        model.load_state_dict(torch.load(args.jpegModelOutput))
        print("JPEG loss: " + str(validate_model(model, torch.utils.data.DataLoader(val_jpeg),
            device, nn.CrossEntropyLoss())))
        print("DPU loss: " + str(validate_model(model, torch.utils.data.DataLoader(val_dpu),
            device, nn.CrossEntropyLoss())))

    if (not args.jpegModelOnly) and (not args.trainOnly):
        model = torch.hub.load('pytorch/vision:v0.10.0', 'resnet18', pretrained=False, num_classes=200)
        model.load_state_dict(torch.load(args.dpuModelOutput))
        print("JPEG loss: " + str(validate_model(model, torch.utils.data.DataLoader(val_jpeg),
            device, nn.CrossEntropyLoss())))
        print("DPU loss: " + str(validate_model(model, torch.utils.data.DataLoader(val_dpu),
            device, nn.CrossEntropyLoss())))

def train_model(device, model, training, validation, savepath="resnet.pth", batch_size=4, epochs=100, patience=8):
    trainloader = torch.utils.data.DataLoader(training, batch_size=batch_size,
                                          shuffle=True)
    val_loader = torch.utils.data.DataLoader(validation, batch_size=batch_size,
                                          shuffle=True)

    if savepath.endswith("/") and not isdir(savepath):
        mkdir(savepath)
    model.train()
    model.to(device)

    criterion = nn.CrossEntropyLoss()
    optimizer = optim.SGD(model.parameters(), lr=0.001, momentum=0.9)

    val_loss = float("inf")
    no_gain_epochs = 0

    if savepath.endswith("/"):
        with open(savepath + "epochs.csv", "w") as f:
            f.write("epoch, training loss, validation loss\n")

    for epoch in range(epochs):
        train_loss = train_epoch(model, trainloader, device, optimizer, criterion, epoch=epoch)
        new_val_loss = validate_model(model, val_loader, device, criterion)
        print(f'[{epoch + 1}, val] loss: {new_val_loss:.3f}\n')
        if new_val_loss < val_loss:
            val_loss = new_val_loss
            no_gain_epochs = 0
        else:
            no_gain_epochs += 1

        if savepath.endswith("/"):
            torch.save(model.state_dict(), savepath + "epoch_" + str(epoch)+".pt")
            with open(savepath + "epochs.csv", "a") as f:
                f.write(str(epoch) + ", " +
                str(train_loss) + ", " +
                str(new_val_loss) + ", " +
                "\n")
        if no_gain_epochs >= patience:
            print("early stopping")
            break

    if not savepath.endswith("/"):
        torch.save(model.state_dict(), savepath)

def train_epoch(model, trainloader, device, optimizer, criterion, epoch=0):
    model.train()
    running_loss = 0.0
    training_loss = []
    for i, data in enumerate(trainloader, 0):
        # get the inputs; data is a list of [inputs, labels]
        inputs, labels = data[0].to(device), data[1].to(device)

        # zero the parameter gradients
        optimizer.zero_grad()

        # forward + backward + optimize
        outputs = model(inputs)
        loss = criterion(outputs, labels)
        loss.backward()
        optimizer.step()

        # print statistics
        loss = loss.item()
        training_loss += [loss]
        running_loss += loss
        if i % 2000 == 1999:    # print every 2000 mini-batches
            print(f'[{epoch + 1}, {i + 1:5d}] loss: {running_loss / 2000:.3f}')
            running_loss = 0.0

    return statistics.mean(training_loss)

def validate_model(model, val_loader, device, criterion):
    model.eval()

    running_loss = []
    for  data in val_loader:
        # get the inputs; data is a list of [inputs, labels]
        inputs, labels = data[0].to(device), data[1].to(device)

        # forward + backward + optimize
        outputs = model(inputs)
        loss = criterion(outputs, labels)
        running_loss += [loss.item()]

    return statistics.mean(running_loss)

def evaluate_model(model, val_loader, criteria, device):
    model.eval()
    running_loss = []*len(criteria)

    for data in val_loader:
        # get the inputs; data is a list of [inputs, labels]
        inputs, labels = data[0].to(device), data[1].to(device)

        # forward + backward + optimize
        outputs = model(inputs)
        for i, criterion in enumerate(criteria):
            loss = criterion(outputs, labels)
            running_loss[i] += [loss.item()]

    return tuple(statistics.mean(running_loss[i]) for i in range(len(criteria)))

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

def commandArgs():
    parser = ArgumentParser(description="Plot image quality metrics csv.")
    parser.add_argument("--evalOnly", "-v", help="only evaluate the model",
        action="store_true")
    parser.add_argument("--trainOnly", "-t", help="only train the model",
        action="store_true")
    parser.add_argument("--jpegModelOnly", "-j", help="only train/evaluate the model the model trained on JPEGs",
        action="store_true")
    parser.add_argument("--dpuModelOnly", "-d", help="only train/evaluate the model the model trained on DPU BMPs",
        action="store_true")
    parser.add_argument("--jpegModelOutput", "-m", help="output directory or file for models trained on JPEGs",
        type=str, default="output/mdl_jpeg/")
    parser.add_argument("--dpuModelOutput", "-n", help="output directory or file for models trained on DPU BMPs",
        type=str, default="output/mdl_dpu/")
    parser.add_argument("--epochs", "-e", help="epochs to run",
        type=int, default=100)
    parser.add_argument("--patience", "-p", help="early stopping patience",
        type=int, default=8)

    args = parser.parse_args()
    if args.jpegModelOnly and args.dpuModelOnly:
        print("Cannot use jpegModelOnly and dpuModelOnly at the same time.")
        parser.print_help()
        exit(0)
    if args.evalOnly and args.trainOnly:
        print("Cannot use evalOnly and trainOnly at the same time.")
        parser.print_help()
        exit(0)
    return args

def reset_weights(model):
    for layer in model.children():
        if hasattr(layer, 'reset_parameters'):
            layer.reset_parameters()

if __name__ == '__main__':
    main()