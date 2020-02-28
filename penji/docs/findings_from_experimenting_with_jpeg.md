# Findings from experimenting with JPEG

## 2020-02-23

* Two useful tools for hexdumps and reverse hexdumps
	- **xxd**: This is a commandline tool that allows you to take hexdumps as well as reverse hexdumps (i.e. back into binary format)
		- Only problem might be if you want to edit the hexdump before converting it back to binary
	- **Okteta**: This is a GUI tool that allows you to open any file and view and edit its hex data, and then generate the binary file from the modified hex data
	- **Wireshark**: already knew this but wireshark is the OG for looking at JPEG headers and finding where they are.

### *Experiment*: Modify the `# of lines` field in the `Start of Frame` header to see if a valid JPEG decoder can put together the image
- Sample Image: 
	- IMG_3009__scale1_1.JPG
- Background: 
	- There is a `Start of Frame` header towards the beginning of the JPEG file that contains metadata about the data that is contained. Some of this metadata includes `Number of lines` which in the sample image is 4000.
- Process:
	- I used okteta to change this number from 4000 to 2000 and generated the binary file.
- Results: 
	- I opened the processed binary file with Gwenview and it shows just the top half of the original image.
	- More importantly, we are still able to put together the JPEG image and display it.
	- **We can modify the `Number of lines` field and still expect a valid jpeg**
- Follow up questions:
	- We need to find a way to cleanly dispose of the unused line data. The overall file is still the same size, we're just not using the all of the information provided.

### *Experiment*: Modify the `Samples per line` field in the `Start of Frame` header to see if a valid JPEG decoder can put together the image
- Sample Image: 
	- IMG_3009__scale1_1.JPG
- Background: 
	- There is a `Start of Frame` header towards the beginning of the JPEG file that contains metadata about the data that is contained. Some of this metadata includes `Samples per line` which in the sample image is 6000.
- Process:
	- I used okteta to change this number from 6000 to 3000 and generated the binary file.
- Results: 
	- I opened the processed binary file with Gwenview and it does not display just the left half of the image.
	- The image produced is tilted and hazy with some lines missing... it does not look good
	- We are not able to put together the JPEG image and display it therefore **we cannot simply modify the `Samples per line` field and expect a valid jpeg**
