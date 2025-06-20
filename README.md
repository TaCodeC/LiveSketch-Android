# LiveSketch

This Android app was developed to be actively used by students during class sessions at my university (unofficial request). While it is still in early development, it is already functional and usable for drawing and testing purposes.

LiveSketch allows users to draw on a multi-layer canvas and stream the result in real time using NDI (Network Device Interface). This makes it suitable for live demos, classroom activities, or experimental creative workflows where remote or shared visuals are needed.
## Manual Setup
To clone and compile the project manually, you must configure the local paths to the NDI SDK, as the required binaries are not included in the repository.

## Requirements / Credits
NDI SDK – Enables real-time streaming of the drawing canvas.
⚠️ Be sure to properly configure the NDI SDK path in your local build setup (CMakeLists.txt) before compiling.


### TODO
Refactor code for clarity and modularity
Restructure project directories
Optimize app lifecycle
Improve performance and resource management
Remove unused or experimental code
