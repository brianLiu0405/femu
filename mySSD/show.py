import tkinter as tk
from tkinter import ttk
from tkinter import filedialog
from tkinter import Toplevel
import os
from timeline import plot_logic_vs_timestamp

class HexAsciiViewer:
    def __init__(self, master):
        self.ch = 0
        self.lun = 0
        self.plane = 0
        self.block = 0
        self.page = 0

        self.master = master
        
        self.file_path = ''
        self.master.title("mySSD Hex and ASCII Viewer")

        self.line_numbers = tk.Text(master, width=4, wrap="none")
        self.hex_text = tk.Text(master, wrap="none")
        self.ascii_text = tk.Text(master, wrap="none")

        self.line_numbers.pack(side="left", fill="y")
        self.hex_text.pack(side="left", fill="both", expand=True)
        self.ascii_text.pack(side="right", fill="both", expand=True)

        # Set yscrollcommand to sync_scroll method
        self.line_numbers.config(yscrollcommand=self.sync_scroll)
        self.hex_text.config(yscrollcommand=self.sync_scroll)
        self.ascii_text.config(yscrollcommand=self.sync_scroll)

        # Bind mouse wheel event to on_mousewheel method
        self.line_numbers.bind("<MouseWheel>", self.on_mousewheel)
        self.hex_text.bind("<MouseWheel>", self.on_mousewheel)
        self.ascii_text.bind("<MouseWheel>", self.on_mousewheel)

        self.file_control_frame = tk.Frame(master)
        self.file_control_frame.pack()

        # Add Load File button
        self.load_button = tk.Button(self.file_control_frame, text="Load File", command=self.load_file)
        self.load_button.pack()
        
        self.ch_frame = tk.Frame(self.file_control_frame, pady=3)
        self.ch_frame.pack()  # Add some padding between label and OptionMenu
        self.ch_label = tk.Label(self.ch_frame, text="ch: ")
        self.ch_label.pack(side="left")
        self.ch_combo = ttk.Combobox(self.ch_frame, height=10, width=5)
        self.ch_combo.pack()
    
        self.lun_frame = tk.Frame(self.file_control_frame, pady=3)
        self.lun_frame.pack()  # Add some padding between label and OptionMenu
        self.lun_label = tk.Label(self.lun_frame, text="lun: ")
        self.lun_label.pack(side="left")
        self.lun_combo = ttk.Combobox(self.lun_frame, height=10, width=5)
        self.lun_combo.pack()
    
        self.plane_frame = tk.Frame(self.file_control_frame, pady=3)
        self.plane_frame.pack()  # Add some padding between label and OptionMenu
        self.plane_label = tk.Label(self.plane_frame, text="plane: ")
        self.plane_label.pack(side="left")
        self.plane_combo = ttk.Combobox(self.plane_frame, height=10, width=5)
        self.plane_combo.pack()
    
        self.block_frame = tk.Frame(self.file_control_frame, pady=3)
        self.block_frame.pack()  # Add some padding between label and OptionMenu
        self.block_label = tk.Label(self.block_frame, text="block: ")
        self.block_label.pack(side="left")
        self.block_combo = ttk.Combobox(self.block_frame, height=10, width=5)
        self.block_combo.pack()
            
        self.page_frame = tk.Frame(self.file_control_frame, pady=3)
        self.page_frame.pack()  # Add some padding between label and OptionMenu
        self.page_label = tk.Label(self.page_frame, text="page: ")
        self.page_label.pack(side="left")
        self.page_combo = ttk.Combobox(self.page_frame, height=10, width=5)
        self.page_combo.pack()

        # Add Load File button
        self.read_button = tk.Button(self.file_control_frame, text="Read Page", command=self.read_page)
        self.read_button.pack()

        open_l2p_button = tk.Button(self.file_control_frame, text="Open L2P", command=self.open_l2p)
        open_l2p_button.pack()

        open_time_button = tk.Button(self.file_control_frame, text="timeline", command=self.open_time)
        open_time_button.pack()

    def sync_scroll(self, *args):
        # Synchronize scrollbars
        self.line_numbers.yview_moveto(args[0])
        self.ascii_text.yview_moveto(args[0])
        self.hex_text.yview_moveto(args[0])

    def on_mousewheel(self, event):
        # Scroll both text widgets on mouse wheel event
        self.line_numbers.yview_scroll(int(-1*(event.delta/120)), "units")
        self.hex_text.yview_scroll(int(-1*(event.delta/120)), "units")
        self.ascii_text.yview_scroll(int(-1*(event.delta/120)), "units")
        return "break"

    def load_files_in_folder(self, folder_path):
        # Load all files in the selected folder
        if folder_path:
            find = 0
            for f in os.listdir(folder_path):
                if f == "param.ini":
                    find = 1
                    with open(os.path.join(folder_path, f), 'r') as file:
                        content = file.read()
                        lines = content.splitlines()
                        for line in lines:
                            if line.startswith("nchs"):
                                self.ch = int(line.split("=")[1])
                            elif line.startswith("luns_per_ch"):
                                self.lun = int(line.split("=")[1])
                            elif line.startswith("pls_per_lun"):
                                self.plane = int(line.split("=")[1])
                            elif line.startswith("blks_per_pl"):
                                self.block = int(line.split("=")[1])
                            elif line.startswith("pgs_per_blk"):
                                self.page = int(line.split("=")[1])

            if find == 0:
                print("ERROR: param.ini not found in the selected folder")

        ch_list = [str(i) for i in range(self.ch)]
        lun_list = [str(i) for i in range(self.lun)]
        plane_list = [str(i) for i in range(self.plane)]
        block_list = [str(i) for i in range(self.block)]
        page_list = [str(i) for i in range(self.page)]

        self.ch_combo['values'] = ch_list
        self.lun_combo['values'] = lun_list
        self.plane_combo['values'] = plane_list
        self.block_combo['values'] = block_list
        self.page_combo['values'] = page_list

    def load_file(self):
        # Open file dialog and load file content
        self.file_path = filedialog.askdirectory()
        if self.file_path:
            self.load_files_in_folder(self.file_path)
            
    def read_page(self):
        ch_val = self.ch_combo.get()
        lun_val = self.lun_combo.get()
        plane_val = self.plane_combo.get()
        block_val = self.block_combo.get()
        page_val = self.page_combo.get()

        self.line_numbers.config(state=tk.NORMAL)
        self.hex_text.config(state=tk.NORMAL)
        self.ascii_text.config(state=tk.NORMAL)

        # struct FG_OOB {
        #     uint64_t LPA;
        #     uint64_t P_PPA;
        #     int64_t Timestamp;
        #     uint32_t RIP;
        #     uint32_t rsv;
        # };
        with open(os.path.join(self.file_path, 'ch'+ch_val, 'lun'+lun_val, 'pl'+plane_val, 'blk'+block_val, 'pg'+page_val), 'rb') as file:
            hex_lines = []
            ascii_lines = []
            line_numbers_lines = []
            content = file.read()
            OOB = content[:32]
            data = content[32:]
            LPA = int.from_bytes(OOB[:8], byteorder='little')
            P_PPA = OOB[8:16]
            Timestamp = int.from_bytes(OOB[16:24], byteorder='little')
            RIP = int.from_bytes(OOB[24:28], byteorder='little')
            # for i in P_PPA : print(i, " ")
            
            block = int.from_bytes(P_PPA[:2], byteorder='little')
            page = int.from_bytes(P_PPA[2:4], byteorder='little')
            plane = int.from_bytes(P_PPA[5:6], byteorder='little')
            lun = int.from_bytes(P_PPA[6:7], byteorder='little')
            ch = int.from_bytes(P_PPA[7:8], byteorder='little') & 0xFE

            formatted_str = "LPA: %s, Timestamp: %d, RIP, %d" % (LPA, Timestamp, RIP)
            hex_lines.append(formatted_str)
            ascii_lines.append(formatted_str)
            line_numbers_lines.append("")
            formatted_str = "ch %d, lun %d, plane %d, block %d, page %d" % (ch, lun, plane, block, page)
            hex_lines.append(formatted_str)
            ascii_lines.append(formatted_str)
            line_numbers_lines.append("")

            for i in range(0, len(data), 16):
                hex_chunk = ' '.join("{:02x}".format(b) for b in data[i:i+16])
                ascii_chunk = ''.join(chr(b) if 32 <= b <= 126 else '.' for b in data[i:i+16])
                line_numbers_chunk = ''.join(str(i//16))
                line_numbers_lines.append(line_numbers_chunk)
                hex_lines.append(hex_chunk)
                ascii_lines.append(ascii_chunk)
            
            self.line_numbers.delete(1.0, tk.END)
            self.hex_text.delete(1.0, tk.END)
            self.ascii_text.delete(1.0, tk.END)
            
            self.line_numbers.insert(tk.END, '\n'.join(line_numbers_lines))
            self.hex_text.insert(tk.END, '\n'.join(hex_lines))
            self.ascii_text.insert(tk.END, '\n'.join(ascii_lines))
        

        self.line_numbers.config(state=tk.DISABLED)
        self.hex_text.config(state=tk.DISABLED)
        self.ascii_text.config(state=tk.DISABLED)
    
    def open_l2p(self):
        l2p_name = filedialog.askopenfilename()
        if l2p_name:
        
            l2p_window = Toplevel(root)
            l2p_window.title("L2P")
            l2p_window.geometry("600x1000")
            
            l2p_table = tk.Text(l2p_window, wrap='word', height=10, width=40)
            l2p_table.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

            l2p_scrollbar = ttk.Scrollbar(l2p_window, orient="vertical", command=l2p_table.yview)
            l2p_scrollbar.pack(side=tk.RIGHT, fill=tk.Y)

            l2p_table.config(yscrollcommand=l2p_scrollbar.set)
            
            with open(l2p_name, 'r', encoding='utf-8') as file:
                l2p_table.delete(1.0, tk.END)
                for line in file:
                    l2p_table.insert(tk.END, line)

            l2p_table.config(state=tk.DISABLED)

    def open_time(self):
        l2p_name = filedialog.askopenfilename()
        plot_logic_vs_timestamp(l2p_name)

    def open_time(self):
        l2p_name = filedialog.askopenfilename()
        plot_logic_vs_timestamp(l2p_name)

if __name__ == "__main__":
    root = tk.Tk()
    root.geometry("1920x1080")
    app = HexAsciiViewer(root)
    root.mainloop()