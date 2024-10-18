import matplotlib
matplotlib.use('TkAgg')  # Use Tkinter-based backend for better compatibility
import matplotlib.pyplot as plt

def plot_logic_vs_timestamp(file_name='L2P_0'):
    """Reads the data from a file and plots Logic vs Timestamp."""
    logic_values = []
    timestamps = []

    # Read and parse the input file
    with open(file_name, 'r') as file:
        for line in file:
            if "physical -1" in line:
                continue  # Skip invalid lines
            
            parts = line.split(',')
            logic_part = parts[0].split()
            timestamp_part = parts[-1].split()

            logic_value = int(logic_part[1].replace(':', ''))
            timestamp_value = int(timestamp_part[1])

            # Store the data
            logic_values.append(logic_value)
            timestamps.append(timestamp_value)

    # Create a figure and axis
    fig, ax = plt.subplots(figsize=(10, 6))

    # Plot the data
    # ax.plot(timestamps, logic_values, marker='o', linestyle='-', markersize=8)
    ax.plot(timestamps, logic_values, marker='o', linestyle='None', markersize=1)

    # Set title and labels
    ax.set_title('Logic vs Timestamp')
    ax.set_xlabel('Timestamp')
    ax.set_ylabel('Logic Value')

    # Add a grid
    ax.grid(True)

    # Show the plot with the toolbar (zoom, pan, reset view)
    plt.show()

def get_same_logic():
    """Reads the data from a file and plots Logic vs Timestamp."""
    logic_values = []
    timestamps = []

    # Read and parse the input file
    with open(file_name, 'r') as file:
        for line in file:
            if "physical -1" in line:
                continue  # Skip invalid lines
            
            parts = line.split(',')
            logic_part = parts[0].split()
            timestamp_part = parts[-1].split()

            logic_value = int(logic_part[1].replace(':', ''))
            timestamp_value = int(timestamp_part[1])

            # Store the data
            logic_values.append(logic_value)
            timestamps.append(timestamp_value)