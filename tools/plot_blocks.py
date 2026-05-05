import pandas as pd
import matplotlib.pyplot as plt

CSV_FILE = "blocks.csv"
OUTPUT_IMAGE = "output_plot.png"
DPI = 300
SMOOTH_WINDOW = 11

df = pd.read_csv(CSV_FILE)

df['insert_mbps'] = df['insert_bps'] / 1e6
df['erase_mbps'] = df['erase_bps'] / 1e6

df['insert_smooth'] = df['insert_mbps'].rolling(window=SMOOTH_WINDOW, center=True, min_periods=1).median()
df['erase_smooth'] = df['erase_mbps'].rolling(window=SMOOTH_WINDOW, center=True, min_periods=1).median()

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

ax1.plot(df.index, df['n_blocks'], linewidth=0.5, color='blue')
ax1.set_xlabel("Operations")
ax1.set_ylabel("Number of blocks")
ax1.set_title("Number of blocks per operation")
ax1.grid(True, linestyle='--', alpha=0.7)

ax2.plot(df.index, df['insert_smooth'], linewidth=0.5, color='green', label='Insert (MB/s)')
ax2.plot(df.index, df['erase_smooth'], linewidth=0.5, color='red', label='Erase (MB/s)')
ax2.set_xlabel("Operations")
ax2.set_ylabel("Throughput (MB/s)")
ax2.set_title("Insert and erase throughput (median smoothed, log scale)")
ax2.set_yscale('log')
ax2.grid(True, linestyle='--', alpha=0.7)
ax2.legend()

plt.tight_layout()
plt.savefig(OUTPUT_IMAGE, dpi=DPI, bbox_inches='tight')
print(f"Plot saved as {OUTPUT_IMAGE}")