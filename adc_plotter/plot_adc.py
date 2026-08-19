import os
import time
import glob
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

def minmax_downsample(df, target_points=5000):
    """
    Min-Max peak-preserving downsampling algorithm.
    Reduces N points down to ~2*target_points while preserving all min/max
    transients, spikes, and peak noise boundaries.
    """
    n = len(df)
    if n <= target_points * 2:
        return df

    chunk_size = max(1, n // target_points)
    num_chunks = n // chunk_size
    usable_len = num_chunks * chunk_size

    adc_reshaped = df['raw_adc'].values[:usable_len].reshape(num_chunks, chunk_size)

    min_idx = np.argmin(adc_reshaped, axis=1) + np.arange(0, usable_len, chunk_size)
    max_idx = np.argmax(adc_reshaped, axis=1) + np.arange(0, usable_len, chunk_size)

    combined_idx = np.empty((num_chunks, 2), dtype=int)
    combined_idx[:, 0] = np.minimum(min_idx, max_idx)
    combined_idx[:, 1] = np.maximum(min_idx, max_idx)
    sorted_idx = np.unique(combined_idx.ravel())

    return df.iloc[sorted_idx].reset_index(drop=True)

def load_single_csv(filepath):
    """Smart fast CSV loader with auto-detection for column headers."""
    start_t = time.time()
    print(f"Loading '{filepath}'...")

    # Load CSV using fast PyArrow engine
    df = pd.read_csv(filepath, engine='pyarrow')
    df.columns = [str(c).strip() for c in df.columns]

    # Check if the CSV has NO header (e.g. first row is already datetime data)
    is_headerless = False
    try:
        pd.to_datetime(df.columns[0], format='ISO8601')
        is_headerless = True
    except Exception:
        pass

    if is_headerless:
        # Re-read CSV without header
        df = pd.read_csv(filepath, engine='pyarrow', header=None)
        df.columns = ['timestamp', 'raw_adc']
    else:
        # Auto-match column names case-insensitively
        col_map = {c.lower(): c for c in df.columns}

        timestamp_col = None
        adc_col = None

        # Look for timestamp column candidates
        for cand in ['timestamp', 'time', 'datetime', 'date', 'ts']:
            if cand in col_map:
                timestamp_col = col_map[cand]
                break

        # Look for ADC column candidates
        for cand in ['raw_adc', 'adc', 'val', 'value', 'raw', 'data', 'reading']:
            if cand in col_map:
                adc_col = col_map[cand]
                break

        # Fallback to positional columns (1st = timestamp, 2nd = ADC)
        if timestamp_col is None and len(df.columns) >= 1:
            timestamp_col = df.columns[0]
        if adc_col is None and len(df.columns) >= 2:
            adc_col = df.columns[1]

        df = df.rename(columns={timestamp_col: 'timestamp', adc_col: 'raw_adc'})

    # Keep only the target 2 columns
    df = df[['timestamp', 'raw_adc']].copy()

    # Convert timestamp column to datetime
    if not pd.api.types.is_datetime64_any_dtype(df['timestamp']):
        df['timestamp'] = pd.to_datetime(df['timestamp'], format='ISO8601')

    # Convert raw_adc column to numeric
    df['raw_adc'] = pd.to_numeric(df['raw_adc'], errors='coerce')

    load_time = time.time() - start_t
    print(f"  Loaded {len(df):,} rows from {os.path.basename(filepath)} in {load_time:.2f}s.")
    return df

def plot_adc_files(file_paths, mode='subplots', downsample_points=5000, save_path=None):
    """
    Plots multiple large ADC CSV files.
    :param file_paths: List of CSV file paths
    :param mode: 'subplots' (stacked) or 'overlay' (single axes)
    :param downsample_points: Target resolution per signal for smooth interactive plotting
    :param save_path: Optional output image file path
    """
    dfs = []
    labels = []

    for path in file_paths:
        df = load_single_csv(path)

        # Apply Min-Max peak preserving downsample
        if downsample_points and len(df) > downsample_points * 2:
            print(f"  Downsampling from {len(df):,} -> ~{downsample_points*2:,} points for smooth rendering...")
            df = minmax_downsample(df, target_points=downsample_points)

        dfs.append(df)
        labels.append(os.path.basename(path))

    plt.style.use('dark_background')
    palette = ['#00E5FF', '#FF4081', '#7C4DFF', '#00E676', '#FFEA00']

    if mode == 'subplots':
        fig, axes = plt.subplots(len(dfs), 1, figsize=(14, 3 * len(dfs)), sharex=True)
        if len(dfs) == 1:
            axes = [axes]

        fig.suptitle('Multi-Channel ADC Signal Analysis', fontsize=15, fontweight='bold', color='#FFFFFF', y=0.98)

        for i, (df, label, ax) in enumerate(zip(dfs, labels, axes)):
            color = palette[i % len(palette)]
            ax.plot(df['timestamp'], df['raw_adc'], label=label, color=color, linewidth=0.8, alpha=0.9)
            ax.set_ylabel('Raw ADC Value', fontsize=10, fontweight='semibold')
            ax.grid(True, linestyle='--', alpha=0.3)
            ax.legend(loc='upper right', framealpha=0.6)
            ax.set_title(f'Channel {i+1}: {label}', fontsize=11, loc='left', color=color)

        axes[-1].set_xlabel('Timestamp (HH:MM:SS.ffffff)', fontsize=11, fontweight='semibold')
        axes[-1].xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S.%f'))

    else:  # Overlay mode
        fig, ax = plt.subplots(figsize=(14, 7))
        fig.suptitle('Overlay ADC Signal Comparison', fontsize=15, fontweight='bold', color='#FFFFFF')

        for i, (df, label) in enumerate(zip(dfs, labels)):
            color = palette[i % len(palette)]
            ax.plot(df['timestamp'], df['raw_adc'], label=label, color=color, linewidth=0.8, alpha=0.7)

        ax.set_xlabel('Timestamp (HH:MM:SS.ffffff)', fontsize=11, fontweight='semibold')
        ax.set_ylabel('Raw ADC Value', fontsize=11, fontweight='semibold')
        ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S.%f'))
        ax.grid(True, linestyle='--', alpha=0.3)
        ax.legend(loc='upper right', frameon=True, facecolor='#1E1E1E')

    fig.autofmt_xdate()
    plt.tight_layout()

    if save_path:
        plt.savefig(save_path, dpi=200, bbox_inches='tight')
        print(f"Plot saved successfully to '{save_path}'")
    else:
        plt.show()

if __name__ == '__main__':
    csv_files = sorted(glob.glob('data/*.csv'))[:3]
    if len(csv_files) < 3:
        print("Error: 3 CSV files not found under data/ directory.")
    else:
        print(f"Plotting files:\n  " + "\n  ".join(csv_files))
        plot_adc_files(csv_files, mode='subplots', downsample_points=5000)
