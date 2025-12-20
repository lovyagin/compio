import json
import numpy as np
import matplotlib.pyplot as plt
import matplotlib as mpl
from pathlib import Path
from collections import defaultdict

# Set up matplotlib for better quality plots
mpl.rcParams['figure.dpi'] = 150
mpl.rcParams['savefig.dpi'] = 300
mpl.rcParams['font.size'] = 10
mpl.rcParams['axes.titlesize'] = 12
mpl.rcParams['axes.labelsize'] = 11

plt.style.use('seaborn-v0_8-whitegrid')

class BenchmarkAnalyzer:
    def __init__(self):
        self.data = {}
        self.n_switch_values = sorted([1, 2, 4, 8, 16, 32, 64, 128, 256, 512])
        
    def load_json_file(self, filepath):
        """Load a JSON benchmark file and process repetition data."""
        try:
            with open(filepath, 'r') as f:
                content = json.load(f)
        except FileNotFoundError:
            return {}
        
        # Group benchmarks by their unique parameters
        grouped_data = defaultdict(list)
        
        for bench in content['benchmarks']:
            if 'run_type' in bench and bench['run_type'] == 'aggregate':
                continue
                
            parts = bench['name'].split('/')
            if len(parts) < 8:
                continue
            
            library = parts[0].split('_')[1]
            is_write = int(parts[1])
            n_switch = int(parts[4])
            
            key = (library, is_write, n_switch)
            grouped_data[key].append(bench)
        
        # Compute statistics for each group
        processed_data = {}
        for key, benches in grouped_data.items():
            if len(benches) < 1:
                continue
                
            real_times = []
            bytes_per_second = []
            block_cache_hits = []
            n_bytes_compressed = []
            n_bytes_decompressed = []
            n_bytes_read = []
            n_bytes_written = []
            file_sizes = []
            
            for bench in benches:
                real_times.append(bench['real_time'])
                bytes_per_second.append(bench['bytes_per_second'])
                
                if 'block_cache_hit' in bench:
                    block_cache_hits.append(bench['block_cache_hit'])
                if 'n_bytes_compressed' in bench:
                    n_bytes_compressed.append(bench['n_bytes_compressed'])
                if 'n_bytes_decompressed' in bench:
                    n_bytes_decompressed.append(bench['n_bytes_decompressed'])
                if 'n_bytes_read' in bench:
                    n_bytes_read.append(bench['n_bytes_read'])
                if 'n_bytes_written' in bench:
                    n_bytes_written.append(bench['n_bytes_written'])
                if 'file_size' in bench:
                    file_sizes.append(bench['file_size'])
            
            processed_data[key] = {
                'library': key[0],
                'is_write': key[1],
                'n_switch': key[2],
                'real_time_mean': np.mean(real_times),
                'real_time_std': np.std(real_times),
                'bytes_per_second_mean': np.mean(bytes_per_second),
                'bytes_per_second_std': np.std(bytes_per_second),
                'block_cache_hit_mean': np.mean(block_cache_hits) if block_cache_hits else 0,
                'n_bytes_compressed_mean': np.mean(n_bytes_compressed) if n_bytes_compressed else 0,
                'n_bytes_decompressed_mean': np.mean(n_bytes_decompressed) if n_bytes_decompressed else 0,
                'n_bytes_read_mean': np.mean(n_bytes_read) if n_bytes_read else 0,
                'n_bytes_written_mean': np.mean(n_bytes_written) if n_bytes_written else 0,
                'file_size_mean': np.mean(file_sizes) if file_sizes else 0,
                'n_repetitions': len(benches)
            }
        
        return processed_data
    
    def load_all_data(self):
        """Load all required JSON files."""
        files = {
            'old_zlib': 'out_old_zlib.json',
            'stdio': 'out_stdio.json',
            'zlib': 'out_zlib.json',
            'lz4': 'out_lz4.json',
            'zstd': 'out_zstd.json'
        }
        
        for name, filename in files.items():
            self.data[name] = self.load_json_file(filename)
    
    def get_data_for_plot(self, library, is_write):
        """Extract and sort data for a specific library and operation type."""
        if library not in self.data:
            return [], []
        
        n_switch_vals = []
        data_points = []
        
        for key, bench_data in self.data[library].items():
            if bench_data['is_write'] == is_write:
                n_switch_vals.append(bench_data['n_switch'])
                data_points.append(bench_data)
        
        sorted_indices = np.argsort(n_switch_vals)
        sorted_n_switch = [n_switch_vals[i] for i in sorted_indices]
        sorted_data = [data_points[i] for i in sorted_indices]
        
        return sorted_n_switch, sorted_data
    
    def setup_cache_hit_axis(self, ax, cache_hits):
        """
        Set up X-axis to emphasize high cache hit rates (near 100%).
        """
        def transform_cache_hit(x):
            if x >= 100:
                return 2.0
            else:
                return -np.log10(100 - x)
        
        transformed_hits = [transform_cache_hit(h) for h in cache_hits]
        
        tick_percentages = [0, 50, 90, 95, 98, 99, 99.5, 99.9, 99.99, 100]
        tick_positions = [transform_cache_hit(p) for p in tick_percentages]
        tick_labels = [f'{p:.2f}' if p >= 99 else f'{p:.0f}' for p in tick_percentages]
        
        ax.set_xticks(tick_positions)
        ax.set_xticklabels(tick_labels, rotation=45, ha='right')
        ax.set_xlabel('Cache Hit Rate (%)')
        
        min_transformed = min(transformed_hits) if transformed_hits else transform_cache_hit(0)
        max_transformed = max(transformed_hits) if transformed_hits else transform_cache_hit(100)
        ax.set_xlim(min_transformed - 0.1, max_transformed + 0.1)
        
        return transformed_hits
    
    def figure1_cache_redesign(self):
        """Create Figure 1: Cache Redesign Improvement (2x2 grid)."""
        fig, axes = plt.subplots(2, 2, figsize=(12, 10))
        fig.suptitle('Figure 1: Cache Redesign Performance Improvement (HTML Data, zlib)', 
                    fontsize=14, fontweight='bold')
        
        # Plot 1A: Compression work for writes
        ax = axes[0, 0]
        n_switch_old, data_old = self.get_data_for_plot('old_zlib', 1)
        n_switch_new, data_new = self.get_data_for_plot('zlib', 1)
        
        if n_switch_old and n_switch_new:
            comp_old = [d['n_bytes_compressed_mean'] / 1e6 for d in data_old]
            comp_new = [d['n_bytes_compressed_mean'] / 1e6 for d in data_new]
            
            ax.plot(n_switch_old, comp_old, 'o-', label='old compio', linewidth=2)
            ax.plot(n_switch_new, comp_new, 's-', label='new compio', linewidth=2)
            ax.set_xscale('log')
            ax.set_xlabel('n_switch (log scale)')
            ax.set_ylabel('Bytes Compressed (MB)')
            ax.set_title('Compression Work: Write Operations')
            ax.grid(True, alpha=0.3)
            ax.legend()
            ax.set_xlim(0.8, 600)
            ax.set_ylim(min(comp_new) - 2, max(comp_old) + 2)
        
        # Plot 1B: Decompression work for reads
        ax = axes[0, 1]
        n_switch_old, data_old = self.get_data_for_plot('old_zlib', 0)
        n_switch_new, data_new = self.get_data_for_plot('zlib', 0)
        
        if n_switch_old and n_switch_new:
            decomp_old = [d['n_bytes_decompressed_mean'] / 1e6 for d in data_old]
            decomp_new = [d['n_bytes_decompressed_mean'] / 1e6 for d in data_new]
            
            ax.plot(n_switch_old, decomp_old, 'o-', label='old compio', linewidth=2)
            ax.plot(n_switch_new, decomp_new, 's--', label='new compio', linewidth=2)
            ax.set_xscale('log')
            ax.set_xlabel('n_switch (log scale)')
            ax.set_ylabel('Bytes Decompressed (MB)')
            ax.set_title('Decompression Work: Read Operations')
            ax.grid(True, alpha=0.3)
            ax.legend()
            ax.set_xlim(0.8, 600)
        
        # Plot 1C: Write performance
        ax = axes[1, 0]
        n_switch_old_write, data_old_write = self.get_data_for_plot('old_zlib', 1)
        n_switch_new_write, data_new_write = self.get_data_for_plot('zlib', 1)
        
        if n_switch_old_write and n_switch_new_write:
            throughput_old = [d['bytes_per_second_mean'] / (1024**3) for d in data_old_write]
            throughput_new = [d['bytes_per_second_mean'] / (1024**3) for d in data_new_write]
            std_old = [d['bytes_per_second_std'] / (1024**3) for d in data_old_write]
            std_new = [d['bytes_per_second_std'] / (1024**3) for d in data_new_write]
            
            ax.errorbar(n_switch_old_write, throughput_old, yerr=std_old, 
                       fmt='o-', label='old compio', capsize=3, linewidth=2)
            ax.errorbar(n_switch_new_write, throughput_new, yerr=std_new, 
                       fmt='s-', label='new compio', capsize=3, linewidth=2)
            ax.set_xscale('log')
            ax.set_xlabel('n_switch (log scale)')
            ax.set_ylabel('Throughput (GiB/s)')
            ax.set_title('Write Performance')
            ax.grid(True, alpha=0.3)
            ax.legend()
            ax.set_xlim(0.8, 600)
        
        # Plot 1D: Read performance
        ax = axes[1, 1]
        n_switch_old_read, data_old_read = self.get_data_for_plot('old_zlib', 0)
        n_switch_new_read, data_new_read = self.get_data_for_plot('zlib', 0)
        
        if n_switch_old_read and n_switch_new_read:
            throughput_old = [d['bytes_per_second_mean'] / (1024**3) for d in data_old_read]
            throughput_new = [d['bytes_per_second_mean'] / (1024**3) for d in data_new_read]
            std_old = [d['bytes_per_second_std'] / (1024**3) for d in data_old_read]
            std_new = [d['bytes_per_second_std'] / (1024**3) for d in data_new_read]
            
            ax.errorbar(n_switch_old_read, throughput_old, yerr=std_old, 
                       fmt='o-', label='old compio', capsize=3, linewidth=2)
            ax.errorbar(n_switch_new_read, throughput_new, yerr=std_new, 
                       fmt='s-', label='new compio', capsize=3, linewidth=2)
            ax.set_xscale('log')
            ax.set_xlabel('n_switch (log scale)')
            ax.set_ylabel('Throughput (GiB/s)')
            ax.set_title('Read Performance')
            ax.grid(True, alpha=0.3)
            ax.legend()
            ax.set_xlim(0.8, 600)
        
        plt.tight_layout()
        plt.savefig('figure1_cache_redesign.png', bbox_inches='tight', dpi=300)
        plt.close()
    
    def figure2_io_reduction(self):
        """Create Figure 2: I/O Reduction Mechanism (1x2 grid)."""
        fig, axes = plt.subplots(1, 2, figsize=(14, 6))
        fig.suptitle('Figure 2: I/O Reduction vs Cache Hit Rate (HTML Data, zlib)', 
                    fontsize=14, fontweight='bold')
        
        # Plot 2A: Write I/O reduction
        ax = axes[0]
        n_switch_compio, data_compio = self.get_data_for_plot('zlib', 1)
        n_switch_stdio, data_stdio = self.get_data_for_plot('stdio', 1)
        
        if n_switch_compio and n_switch_stdio:
            io_ratios = []
            cache_hits = []
            
            for i, n_switch in enumerate(n_switch_compio):
                if n_switch in n_switch_stdio:
                    stdio_idx = n_switch_stdio.index(n_switch)
                    compio_bytes = data_compio[i]['n_bytes_written_mean']
                    stdio_bytes = data_stdio[stdio_idx]['n_bytes_written_mean']
                    
                    if stdio_bytes > 0:
                        io_ratio = compio_bytes / stdio_bytes
                        io_ratios.append(io_ratio)
                        cache_hits.append(data_compio[i]['block_cache_hit_mean'] * 100)
            
            if io_ratios:
                sorted_indices = np.argsort(cache_hits)
                cache_hits = [cache_hits[i] for i in sorted_indices]
                io_ratios = [io_ratios[i] for i in sorted_indices]
                
                transformed_hits = self.setup_cache_hit_axis(ax, cache_hits)
                
                ax.plot(transformed_hits, io_ratios, 'o-', linewidth=2)
                ax.axhline(y=1.0, color='r', linestyle='--', alpha=0.5, label='Logical bytes')
                ax.set_ylabel('I/O Write Ratio\n(compio / stdio)')
                ax.set_title('Write I/O Reduction')
                ax.grid(True, alpha=0.3)
                ax.legend()
        
        # Plot 2B: Read I/O reduction
        ax = axes[1]
        n_switch_compio, data_compio = self.get_data_for_plot('zlib', 0)
        n_switch_stdio, data_stdio = self.get_data_for_plot('stdio', 0)
        
        if n_switch_compio and n_switch_stdio:
            io_ratios = []
            cache_hits = []
            
            for i, n_switch in enumerate(n_switch_compio):
                if n_switch in n_switch_stdio:
                    stdio_idx = n_switch_stdio.index(n_switch)
                    compio_bytes = data_compio[i]['n_bytes_read_mean']
                    stdio_bytes = data_stdio[stdio_idx]['n_bytes_read_mean']
                    
                    if stdio_bytes > 0:
                        io_ratio = compio_bytes / stdio_bytes
                        io_ratios.append(io_ratio)
                        cache_hits.append(data_compio[i]['block_cache_hit_mean'] * 100)
            
            if io_ratios:
                sorted_indices = np.argsort(cache_hits)
                cache_hits = [cache_hits[i] for i in sorted_indices]
                io_ratios = [io_ratios[i] for i in sorted_indices]
                
                transformed_hits = self.setup_cache_hit_axis(ax, cache_hits)
                
                ax.plot(transformed_hits, io_ratios, 'o-', linewidth=2)
                ax.axhline(y=1.0, color='r', linestyle='--', alpha=0.5, label='Logical bytes')
                ax.set_ylabel('I/O Read Ratio\n(compio / stdio)')
                ax.set_title('Read I/O Reduction')
                ax.grid(True, alpha=0.3)
                ax.legend()
        
        plt.tight_layout()
        plt.savefig('figure2_io_reduction.png', bbox_inches='tight', dpi=300)
        plt.close()
    
    def figure3_algorithm_comparison(self):
        """Create Figure 3: Algorithm Comparison vs Stdio (1x2 grid)."""
        fig, axes = plt.subplots(1, 2, figsize=(14, 6))
        fig.suptitle('Figure 3: Algorithm Comparison vs Stdio (HTML Data, All Compressors)', 
                    fontsize=14, fontweight='bold')
        
        algorithms = ['zlib', 'lz4', 'zstd']
        markers = {'zlib': 'o', 'lz4': 's', 'zstd': '^'}
        
        # Plot 3A: Read break-even
        ax = axes[0]
        
        for algo in algorithms:
            if algo in self.data:
                n_switch_compio, data_compio = self.get_data_for_plot(algo, 0)
                n_switch_stdio, data_stdio = self.get_data_for_plot('stdio', 0)
                
                if n_switch_compio and n_switch_stdio:
                    throughput_ratios = []
                    cache_hits = []
                    
                    for i, n_switch in enumerate(n_switch_compio):
                        if n_switch in n_switch_stdio:
                            stdio_idx = n_switch_stdio.index(n_switch)
                            compio_throughput = data_compio[i]['bytes_per_second_mean']
                            stdio_throughput = data_stdio[stdio_idx]['bytes_per_second_mean']
                            
                            if stdio_throughput > 0:
                                ratio = compio_throughput / stdio_throughput
                                throughput_ratios.append(ratio)
                                cache_hits.append(data_compio[i]['block_cache_hit_mean'] * 100)
                    
                    if throughput_ratios:
                        sorted_indices = np.argsort(cache_hits)
                        cache_hits = [cache_hits[i] for i in sorted_indices]
                        throughput_ratios = [throughput_ratios[i] for i in sorted_indices]
                        
                        transformed_hits = self.setup_cache_hit_axis(ax, cache_hits)
                        
                        ax.plot(transformed_hits, throughput_ratios, 
                               marker=markers[algo], label=algo, linewidth=2)
        
        ax.axhline(y=1.0, color='r', linestyle='--', alpha=0.7, label='Break-even')
        ax.set_ylabel('Throughput Ratio\n(compio / stdio)')
        ax.set_title('Read Performance vs Stdio')
        ax.grid(True, alpha=0.3)
        ax.legend()
        
        # Plot 3B: Write break-even
        ax = axes[1]
        
        for algo in algorithms:
            if algo in self.data:
                n_switch_compio, data_compio = self.get_data_for_plot(algo, 1)
                n_switch_stdio, data_stdio = self.get_data_for_plot('stdio', 1)
                
                if n_switch_compio and n_switch_stdio:
                    throughput_ratios = []
                    cache_hits = []
                    
                    for i, n_switch in enumerate(n_switch_compio):
                        if n_switch in n_switch_stdio:
                            stdio_idx = n_switch_stdio.index(n_switch)
                            compio_throughput = data_compio[i]['bytes_per_second_mean']
                            stdio_throughput = data_stdio[stdio_idx]['bytes_per_second_mean']
                            
                            if stdio_throughput > 0:
                                ratio = compio_throughput / stdio_throughput
                                throughput_ratios.append(ratio)
                                cache_hits.append(data_compio[i]['block_cache_hit_mean'] * 100)
                    
                    if throughput_ratios:
                        sorted_indices = np.argsort(cache_hits)
                        cache_hits = [cache_hits[i] for i in sorted_indices]
                        throughput_ratios = [throughput_ratios[i] for i in sorted_indices]
                        
                        transformed_hits = self.setup_cache_hit_axis(ax, cache_hits)
                        
                        ax.plot(transformed_hits, throughput_ratios, 
                               marker=markers[algo], label=algo, linewidth=2)
        
        ax.axhline(y=1.0, color='r', linestyle='--', alpha=0.7, label='Break-even')
        ax.set_ylabel('Throughput Ratio\n(compio / stdio)')
        ax.set_title('Write Performance vs Stdio')
        ax.grid(True, alpha=0.3)
        ax.legend()
        
        plt.tight_layout()
        plt.savefig('figure3_algorithm_comparison.png', bbox_inches='tight', dpi=300)
        plt.close()
    
    def figure4_cache_behavior(self):
        """Create Figure 4: Cache Utilization vs Access Locality."""
        fig, ax = plt.subplots(1, 1, figsize=(8, 6))
        fig.suptitle('Figure 4: Cache Utilization vs Access Locality (HTML Data, zlib)', 
                    fontsize=14, fontweight='bold')
        
        n_switch_read, data_read = self.get_data_for_plot('zlib', 0)
        n_switch_write, data_write = self.get_data_for_plot('zlib', 1)
        
        if n_switch_read:
            cache_hits_read = [d['block_cache_hit_mean'] * 100 for d in data_read]
            ax.plot(n_switch_read, cache_hits_read, 'o-', label='Read operations', linewidth=2)
        
        if n_switch_write:
            cache_hits_write = [d['block_cache_hit_mean'] * 100 for d in data_write]
            ax.plot(n_switch_write, cache_hits_write, 's-', label='Write operations', linewidth=2)
        
        ax.set_xscale('log')
        ax.set_xlabel('n_switch (log scale)')
        ax.set_ylabel('Block Cache Hit Rate (%)')
        ax.set_title('Cache Utilization vs Access Locality')
        ax.grid(True, alpha=0.3)
        ax.legend()
        ax.set_xlim(0.8, 600)
        ax.set_ylim(0, 100)
        
        plt.tight_layout()
        plt.savefig('figure4_cache_behavior.png', bbox_inches='tight', dpi=300)
        plt.close()
    
    def run_analysis(self):
        """Main analysis pipeline."""
        self.load_all_data()
        self.figure1_cache_redesign()
        self.figure2_io_reduction()
        self.figure3_algorithm_comparison()
        self.figure4_cache_behavior()

def main():
    analyzer = BenchmarkAnalyzer()
    analyzer.run_analysis()

if __name__ == "__main__":
    main()