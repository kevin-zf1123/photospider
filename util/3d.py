import cv2
import numpy as np
import matplotlib.pyplot as plt
from matplotlib import cm
from mpl_toolkits.mplot3d import Axes3D
import argparse
import sys
import os

def view_heightmap(image_path, downsample_factor=1):
    """
    Loads an image and displays it as an interactive 3D height map.

    Args:
        image_path (str): The path to the height map image file.
        downsample_factor (int): Factor to downsample the image for performance.
                                 A factor of 2 will reduce dimensions by half.
    """
    # 1. Check if the file exists
    if not os.path.exists(image_path):
        print(f"Error: The file was not found at the specified path: {image_path}", file=sys.stderr)
        sys.exit(1)

    # 2. Load the image in grayscale
    # Grayscale is used because color is irrelevant for height; only intensity matters.
    height_map = cv2.imread(image_path, cv2.IMREAD_GRAYSCALE)

    if height_map is None:
        print(f"Error: Could not load or decode the image from path: {image_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Image loaded successfully. Original dimensions: {height_map.shape[1]}x{height_map.shape[0]}")

    # 3. Downsample the image for performance if a factor > 1 is provided
    # This is crucial for rendering large images smoothly.
    if downsample_factor > 1:
        new_width = height_map.shape[1] // downsample_factor
        new_height = height_map.shape[0] // downsample_factor
        if new_width == 0 or new_height == 0:
            print(f"Error: Downsample factor ({downsample_factor}) is too large for image dimensions.", file=sys.stderr)
            sys.exit(1)
        height_map = cv2.resize(height_map, (new_width, new_height), interpolation=cv2.INTER_AREA)
        print(f"Downsampled to: {new_width}x{new_height}")

    # 4. Create a grid of X and Y coordinates
    # These will correspond to the pixel locations.
    h, w = height_map.shape
    x_coords = np.arange(w)
    y_coords = np.arange(h)
    X, Y = np.meshgrid(x_coords, y_coords)

    # The image's pixel intensity will be our Z-axis (height).
    Z = height_map

    # 5. Create the 3D plot
    print("Preparing 3D plot...")
    fig = plt.figure(figsize=(12, 8))
    ax = fig.add_subplot(111, projection='3d')

    # Plot the surface using a colormap to indicate height.
    surf = ax.plot_surface(X, Y, Z, cmap=cm.terrain, linewidth=0, antialiased=False)

    # 6. Customize the plot for better readability and orientation
    ax.set_title(f'3D Height Map: {os.path.basename(image_path)}', fontsize=16)
    ax.set_xlabel('Width (X-axis)')
    ax.set_ylabel('Depth (Y-axis)')
    ax.set_zlabel('Height / Intensity (Z-axis)')

    # Invert the Y-axis to match the standard image coordinate system (0,0 at top-left)
    ax.invert_yaxis()

    # Add a color bar to serve as a legend for the height values
    fig.colorbar(surf, shrink=0.6, aspect=10, label='Pixel Intensity')

    print("Displaying interactive 3D plot. You can click and drag to rotate it.")
    print("Close the plot window to exit the script.")
    plt.show()

if __name__ == '__main__':
    # Set up the command-line argument parser
    parser = argparse.ArgumentParser(
        description="Display an image file (e.g., a PNG or TIFF) as an interactive 3D height map.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "image_path",
        help="Path to the image file to be visualized (e.g., 'cache/35/final_output.png')."
    )
    parser.add_argument(
        "--downsample",
        type=int,
        default=1,
        help="Integer factor to downsample the image by, for better performance on large images.\n"
             "For example, a value of 4 will reduce each dimension by a factor of 4.\n"
             "Default: 1 (no downsampling)."
    )

    args = parser.parse_args()

    view_heightmap(args.image_path, args.downsample)