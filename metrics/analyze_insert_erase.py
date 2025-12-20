import pandas as pd
import matplotlib.pyplot as plt
import matplotlib as mpl
import numpy as np
from pathlib import Path
import argparse

# Set matplotlib style and parameters as specified
plt.style.use('seaborn-v0_8-whitegrid')
mpl.rcParams['figure.dpi'] = 150
mpl.rcParams['savefig.dpi'] = 300
mpl.rcParams['font.size'] = 10
mpl.rcParams['axes.titlesize'] = 12
mpl.rcParams['axes.labelsize'] = 11

def smooth_data(data, window_size=10):
    """Apply moving average smoothing to data."""
    if window_size > len(data):
        window_size = len(data)
    # Use simple moving average
    return pd.Series(data).rolling(window=window_size, center=True, min_periods=1).mean()

def format_bps_to_human(bps_value):
    """Convert bytes per second to human-readable string with appropriate unit."""
    if bps_value >= 1024**4:  # 1 TiB/s
        return f"{bps_value / 1024**4:.2f} TiB/s"
    elif bps_value >= 1024**3:  # 1 GiB/s
        return f"{bps_value / 1024**3:.2f} GiB/s"
    elif bps_value >= 1024**2:  # 1 MiB/s
        return f"{bps_value / 1024**2:.2f} MiB/s"
    elif bps_value >= 1024:  # 1 KiB/s
        return f"{bps_value / 1024:.2f} KiB/s"
    else:
        return f"{bps_value:.2f} B/s"

def get_appropriate_unit(bps_value):
    """Determine the most appropriate unit for a BPS value."""
    if bps_value >= 1024**4:  # 1 TiB/s
        return "TiB/s", 1024**4
    elif bps_value >= 1024**3:  # 1 GiB/s
        return "GiB/s", 1024**3
    elif bps_value >= 1024**2:  # 1 MiB/s
        return "MiB/s", 1024**2
    elif bps_value >= 1024:  # 1 KiB/s
        return "KiB/s", 1024
    else:
        return "B/s", 1

def create_plots(input_csv, output_file=None, smooth_window=3):
    """
    Create a 2x1 grid of plots and optionally save to file.
    
    Parameters:
    -----------
    input_csv : str or Path
        Path to the input CSV file
    output_file : str or None
        Output filename for saving the plot (if None, don't save)
    smooth_window : int
        Window size for smoothing (moving average)
    """
    
    # Read CSV file
    df = pd.read_csv(input_csv)
    
    # Create n_operations (simply the index/row number)
    df['n_operations'] = range(1, len(df) + 1)
    
    # Create figure with 2x1 subplots
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(8, 10), sharex=True, layout='constrained')

    fig.suptitle("Insert/Erase Fragmentation Issues", fontsize=18)
    
    # Plot 1: Number of blocks by n_operations
    ax1.plot(df['n_operations'], df['n_blocks'], linewidth=1)
    
    ax1.set_ylabel('Number of Blocks')
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: Performance by n_operations
    # Smooth the performance data
    insert_smoothed = smooth_data(df['insert_bps'], smooth_window)
    erase_smoothed = smooth_data(df['erase_bps'], smooth_window)
    
    # Plot smoothed data without markers
    ax2.plot(df['n_operations'], insert_smoothed, 
             linewidth=1, alpha=0.8, 
             label='Insert performance')
    
    ax2.plot(df['n_operations'], erase_smoothed, 
             linewidth=1, alpha=0.8,
             label='Erase perfomance')
    
    # Determine appropriate unit for y-axis based on max value
    max_perf = max(insert_smoothed.max(), erase_smoothed.max())
    unit, divisor = get_appropriate_unit(max_perf)
    
    # Format y-axis ticks
    from matplotlib.ticker import FuncFormatter
    def format_func(x, pos):
        return f'{x/divisor:.1f}'
    
    ax2.yaxis.set_major_formatter(FuncFormatter(format_func))
    
    ax2.set_xlabel('Number of operations')
    ax2.set_ylabel(f'Performance ({unit})')
    ax2.grid(True, alpha=0.3)
    ax2.legend(loc='best')
    
    # Adjust layout
    # plt.tight_layout()
    
    # Show plot
    plt.show()
    
    # Save plot if output file is specified
    if output_file:
        fig.savefig(output_file, bbox_inches='tight')
        print(f"Plot saved to: {output_file}")
    
    plt.close(fig)

def main():
    parser = argparse.ArgumentParser(description='Create performance and fragmentation plots from CSV data.')
    parser.add_argument('input', help='Input CSV file path')
    parser.add_argument('-o', '--output', default=None, help='Output PNG file path (optional)')
    parser.add_argument('-s', '--smooth', type=int, default=3, 
                       help='Smoothing window size (default: 3)')
    
    args = parser.parse_args()
    
    # Check if input file exists
    if not Path(args.input).exists():
        print(f"Error: Input file '{args.input}' not found!")
        exit(1)
    
    try:
        create_plots(args.input, args.output, args.smooth)
        print(f"Plots created successfully!")
        if args.output:
            print(f"Plot saved to: {args.output}")
        else:
            print("Plot displayed but not saved (no output file specified)")
    except Exception as e:
        print(f"Error creating plots: {e}")
        exit(1)

if __name__ == "__main__":
    main()