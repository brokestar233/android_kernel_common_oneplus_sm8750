import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
import re
import os
from typing import Dict, List, Optional, Tuple
from uuid import uuid4

class DTSNode:
    """DTS节点类"""
    def __init__(self, name: str, parent=None):
        self.name = name
        self.parent = parent
        self.children = {}
        self.properties = {}
        # 确保 full_path 在创建时正确设置
        self.full_path = self._get_full_path()
    
    def _get_full_path(self) -> str:
        """获取节点完整路径"""
        if self.parent is None:
            # 这是虚拟根节点，它的路径为空
            return "" 
        # 如果父节点是虚拟根节点，并且当前节点不是虚拟根节点本身
        if self.parent and self.parent.name == "root" and self.name != "root": 
            # 顶级DTS节点（如 /soc）的路径应该以一个斜杠开头，然后是节点名
            return f"/{self.name}"
        # 对于任何其他嵌套节点，将其名称附加到父节点的完整路径中
        # parent.full_path 已经包含开头的斜杠（如果它是顶级节点）
        if self.parent:
            # 对于虚拟根节点本身，它的full_path是空字符串，因此拼接时要注意
            if self.parent.full_path == "":
                return f"/{self.name}"
            else:
                return f"{self.parent.full_path}/{self.name}"
        return "" # 理论上不会走到这里，除非是孤立节点
    
    def add_child(self, child):
        """添加子节点"""
        self.children[child.name] = child
        child.parent = self
        child.full_path = child._get_full_path() # 为子节点重新计算 full_path
    
    def remove_child(self, name: str):
        """删除子节点"""
        if name in self.children:
            del self.children[name]
    
    def set_property(self, key: str, value: str):
        """设置属性"""
        self.properties[key] = value
    
    def remove_property(self, key: str):
        """删除属性"""
        if key in self.properties:
            del self.properties[key]
    
    def get_property_path(self, key: str) -> str:
        """获取属性完整路径"""
        # 如果节点本身是虚拟根节点 (理论上DTS中根节点不会直接有属性，除非是 /property 这种特殊情况)
        if self.full_path == "": 
            # 根据补丁格式，根节点属性路径会是 /属性名 (例如 /author)
            return f"/{key}" 
        return f"{self.full_path}/{key}"

class DTSParser:
    """DTS文件解析器"""
    
    def __init__(self):
        self.root = DTSNode("root")
    
    def parse_file(self, file_path: str) -> DTSNode:
        """解析DTS文件"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            return self.parse_content(content)
        except Exception as e:
            raise Exception(f"解析文件失败: {e}")
    
    def parse_content(self, content: str) -> DTSNode:
        """解析DTS内容"""
        # 移除C风格的注释 /* ... */
        content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
        # 移除C++风格的行注释 // ...
        content = re.sub(r'//[^\n]*', '', content)
        
        content = content.strip()

        self.root = DTSNode("root") # 这是一个虚拟根节点，用于承载DTS文件的顶级节点
        
        # 查找最外层的 { ... }; 块内容
        # 这样做可以正确处理 DTS 文件以 "/ { ... };" 或 "{ ... };" 开头的情况。
        brace_start = content.find('{')
        if brace_start == -1:
            # 如果没有找到开头的 '{'，则直接将整个内容作为节点/属性进行解析（作为回退）
            # 这可能表示DTS文件格式异常，或者只是一个DTS片段没有最外层大括号。
            self._parse_node(content, self.root)
            return self.root
        
        brace_count = 1
        i = brace_start + 1
        while i < len(content) and brace_count > 0:
            if content[i] == '{':
                brace_count += 1
            elif content[i] == '}':
                brace_count -= 1
            i += 1
        
        if brace_count == 0:
            # 成功找到最外层大括号块。提取其内部内容。
            outermost_block_content = content[brace_start + 1 : i - 1].strip()
            # 这里的解析逻辑需要修改，以便能够解析 / { ... } 内部的顶级属性和节点
            # DTSNode("root") 扮演的是 "/" 节点，其子节点是 /soc, /memory 等
            # 其属性是 /author, /model 等
            
            # 对于 / { ... }; 这种结构，实际上整个文件内容都是根节点的子内容
            # 所以直接解析 outermost_block_content 到 self.root
            # 确保 DTSNode 的 full_path 正确计算
            self._parse_node(outermost_block_content, self.root)
        else:
            # 大括号不匹配或其他解析错误，作为回退直接处理原始内容。
            self._parse_node(content, self.root)

        return self.root
    
    def _parse_node(self, content: str, parent: DTSNode):
        """递归解析节点"""
        i = 0
        while i < len(content):
            # 跳过空白字符
            while i < len(content) and content[i].isspace():
                i += 1
            
            if i >= len(content):
                break
            
            # 如果遇到闭合大括号 '}'，表示当前节点内容结束
            if content[i] == '}':
                i += 1
                continue
            
            # 寻找节点名或属性名
            start = i
            # 允许节点/属性名包含常见的DTS字符，包括 '#' (现在对于 #address-cells 这种属性很重要)
            while i < len(content) and content[i] not in ' \t\n{=;':
                i += 1
            
            if start == i: # 防止无限循环，如果 'i' 没有前进
                i += 1
                continue
                
            name = content[start:i].strip()
            
            # 跳过名称后的空白字符
            while i < len(content) and content[i].isspace():
                i += 1
            
            if i >= len(content):
                break
            
            if content[i] == '{':
                # 这是一个节点
                # 特殊处理：如果解析到名为 "/" 的节点，并且它的父节点是虚拟根节点
                # 这意味着我们正在解析 DTS 文件中的实际根节点定义，其内容应该附加到虚拟根节点
                if name == "/" and parent.name == "root":
                    node = parent # 将内容解析到虚拟根节点
                else:
                    node = DTSNode(name, parent)
                    parent.add_child(node)
                
                # 查找此节点的匹配闭合大括号 '}'
                i += 1
                brace_count = 1
                node_start = i
                
                while i < len(content) and brace_count > 0:
                    if content[i] == '{':
                        brace_count += 1
                    elif content[i] == '}':
                        brace_count -= 1
                    i += 1
                
                # 递归解析节点内容
                if brace_count == 0:
                    node_content = content[node_start:i-1]
                    self._parse_node(node_content, node)
            
            elif content[i] == '=':
                # 这是一个属性
                i += 1
                # 跳过空白字符
                while i < len(content) and content[i].isspace():
                    i += 1
                
                # 读取属性值
                value_start = i
                if i < len(content) and content[i] == '<':
                    # 十六进制值或数组 (例如 <0x01 0x02>)
                    while i < len(content) and content[i] != '>':
                        i += 1
                    if i < len(content) and content[i] == '>': # 消耗闭合 '>'
                        i += 1 
                elif i < len(content) and content[i] == '"':
                    # 字符串值 (例如 "hello")
                    i += 1 # 跳过开头的 '"'
                    while i < len(content):
                        # 检查非转义的引号
                        if content[i] == '"' and (i == 0 or content[i-1] != '\\'): 
                            break
                        i += 1
                    if i < len(content) and content[i] == '"': # 消耗闭合 '"'
                        i += 1 
                else:
                    # 其他值 (例如整数、布尔值、引用、或没有引号的原始字符串)
                    # 读取直到分号 ';' 或大括号 '{' 或 '}' (新节点或节点结束)
                    while i < len(content) and content[i] not in ';{}': 
                        i += 1
                
                value = content[value_start:i].strip() # 获取原始值，去除首尾空白
                parent.set_property(name, value)
                
                # 跳过分号和其后的任何空白字符，直到下一个有效标记
                while i < len(content) and content[i].isspace():
                    i += 1
                if i < len(content) and content[i] == ';':
                    i += 1
                # 跳过分号后的任何剩余空白字符
                while i < len(content) and content[i].isspace():
                    i += 1
            else:
                # 遇到意外字符，前进以避免无限循环
                i += 1

class PropertyEditDialog(simpledialog.Dialog):
    """自定义属性编辑对话框，支持多行输入"""
    def __init__(self, parent, title, prompt, initialvalue=""):
        self.prompt = prompt
        self.initialvalue = initialvalue
        super().__init__(parent, title)

    def body(self, master):
        ttk.Label(master, text=self.prompt).pack(padx=5, pady=5, anchor=tk.W)
        self.text_widget = tk.Text(master, wrap="word", width=60, height=10, font=('TkDefaultFont', 10))
        self.text_widget.insert(tk.END, self.initialvalue)
        self.text_widget.pack(padx=5, pady=5)
        self.text_widget.focus_set()
        return self.text_widget # initial focus

    def apply(self):
        self.result = self.text_widget.get("1.0", tk.END).strip()

class DTSEditor:
    """DTS编辑器主类"""
    
    def __init__(self, root):
        self.root = root
        self.dts_root = None
        self.original_dts_root = None  # 保存原始DTS树用于补丁生成
        self.current_file = None
        # 定义一个特殊的Treeview ID 来表示虚拟根节点
        self.VIRTUAL_ROOT_ID = "virtual_root_node" 
        self.setup_ui()
    
    def setup_ui(self):
        """设置用户界面"""
        self.root.title("DTS可视化编辑器")
        self.root.geometry("1200x800")
        
        # 创建菜单栏
        self.create_menu()
        
        # 创建主框架
        main_frame = ttk.Frame(self.root)
        main_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # 创建左侧树形视图
        self.create_tree_view(main_frame)
        
        # 创建右侧属性编辑面板
        self.create_property_panel(main_frame)
        
        # 创建搜索框
        self.create_search_panel(self.root) 
    
    def create_menu(self):
        """创建菜单栏"""
        menubar = tk.Menu(self.root, tearoff=0, bg="#f0f0f0", fg="black") 
        self.root.config(menu=menubar)
        
        # 文件菜单
        file_menu = tk.Menu(menubar, tearoff=0, bg="#f0f0f0", fg="black")
        menubar.add_cascade(label="文件", menu=file_menu)
        file_menu.add_command(label="打开DTS文件", command=self.open_file)
        file_menu.add_separator()
        file_menu.add_command(label="保存为DTS", command=self.save_as_dts)
        file_menu.add_command(label="生成补丁", command=self.generate_patch)
        file_menu.add_separator()
        file_menu.add_command(label="加载补丁", command=self.load_patch)
        file_menu.add_separator()
        file_menu.add_command(label="退出", command=self.root.quit)
        
        # 编辑菜单
        edit_menu = tk.Menu(menubar, tearoff=0, bg="#f0f0f0", fg="black")
        menubar.add_cascade(label="编辑", menu=edit_menu)
        edit_menu.add_command(label="添加节点", command=self.add_node)
        edit_menu.add_command(label="删除节点", command=self.delete_node)
        edit_menu.add_command(label="添加属性", command=self.add_property)
        edit_menu.add_command(label="删除属性", command=self.delete_property)

        # 比较菜单
        compare_menu = tk.Menu(menubar, tearoff=0, bg="#f0f0f0", fg="black")
        menubar.add_cascade(label="比较", menu=compare_menu)
        compare_menu.add_command(label="生成DTS差异补丁", command=self.generate_diff_patch)
    
    def create_tree_view(self, parent):
        """创建树形视图"""
        tree_frame = ttk.Frame(parent)
        tree_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        # 树形控件
        self.tree = ttk.Treeview(tree_frame, columns=('type',), show='tree headings')
        self.tree.heading('#0', text='节点/属性')
        self.tree.heading('type', text='类型')
        self.tree.column('type', width=100)
        
        # 滚动条
        tree_scroll = ttk.Scrollbar(tree_frame, orient=tk.VERTICAL, command=self.tree.yview)
        self.tree.configure(yscrollcommand=tree_scroll.set)
        
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        # 绑定事件
        self.tree.bind('<<TreeviewSelect>>', self.on_tree_select)
        self.tree.bind('<Double-1>', self.on_tree_double_click)
    
    def create_property_panel(self, parent):
        """创建属性编辑面板"""
        prop_frame = ttk.LabelFrame(parent, text="属性编辑")
        prop_frame.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=(5, 0))
        
        # 节点信息
        info_frame = ttk.Frame(prop_frame)
        info_frame.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Label(info_frame, text="节点路径:").pack(anchor=tk.W)
        self.path_var = tk.StringVar()
        self.path_entry = ttk.Entry(info_frame, textvariable=self.path_var, state='readonly')
        self.path_entry.pack(fill=tk.X, pady=(0, 5))
        
        # 属性列表
        ttk.Label(info_frame, text="属性:").pack(anchor=tk.W)
        
        prop_list_frame = ttk.Frame(info_frame)
        prop_list_frame.pack(fill=tk.BOTH, expand=True)
        
        self.prop_tree = ttk.Treeview(prop_list_frame, columns=('value',), show='tree headings')
        self.prop_tree.heading('#0', text='属性名')
        self.prop_tree.heading('value', text='值')
        
        prop_scroll = ttk.Scrollbar(prop_list_frame, orient=tk.VERTICAL, command=self.prop_tree.yview)
        self.prop_tree.configure(yscrollcommand=prop_scroll.set)
        
        self.prop_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        prop_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        # 绑定双击编辑
        self.prop_tree.bind('<Double-1>', self.edit_property_value)
        
        # 按钮框架
        button_frame = ttk.Frame(prop_frame)
        button_frame.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Button(button_frame, text="添加属性", command=self.add_property).pack(side=tk.LEFT, padx=(0, 5))
        ttk.Button(button_frame, text="删除属性", command=self.delete_property).pack(side=tk.LEFT)
    
    def create_search_panel(self, parent):
        """创建搜索面板"""
        search_frame = ttk.LabelFrame(parent, text="搜索")
        search_frame.pack(fill=tk.X, padx=5, pady=5)
        
        entry_frame = ttk.Frame(search_frame)
        entry_frame.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Label(entry_frame, text="搜索:").pack(side=tk.LEFT)
        self.search_var = tk.StringVar()
        self.search_entry = ttk.Entry(entry_frame, textvariable=self.search_var)
        self.search_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(5, 0))
        
        ttk.Button(entry_frame, text="搜索节点", command=self.search_nodes).pack(side=tk.LEFT, padx=(5, 0))
        ttk.Button(entry_frame, text="搜索属性", command=self.search_properties).pack(side=tk.LEFT, padx=(5, 0))
        
        # 绑定回车键
        self.search_entry.bind('<Return>', lambda e: self.search_nodes())
    
    def open_file(self):
        """打开DTS文件"""
        file_path = filedialog.askopenfilename(
            title="选择DTS文件",
            filetypes=[("DTS files", "*.dts"), ("All files", "*.*")]
        )
        
        if file_path:
            try:
                parser = DTSParser()
                self.dts_root = parser.parse_file(file_path)
                # 深拷贝dts_root，以便在修改后仍能与原始树比较
                self.original_dts_root = self._deep_copy_dts_node(self.dts_root) 
                self.current_file = file_path
                self.populate_tree()
                messagebox.showinfo("成功", "DTS文件加载成功!")
            except Exception as e:
                messagebox.showerror("错误", f"加载DTS文件失败: {e}")

    def _deep_copy_dts_node(self, node: DTSNode, parent=None) -> DTSNode:
        """深度复制DTSNode及其子节点和属性"""
        new_node = DTSNode(node.name, parent)
        # 复制属性字典，确保是新的字典
        new_node.properties = node.properties.copy() 
        for child_name, child_node in node.children.items():
            new_child = self._deep_copy_dts_node(child_node, new_node)
            new_node.add_child(new_child)
        return new_node
    
    def populate_tree(self):
        """填充树形视图"""
        # 清空树
        for item in self.tree.get_children():
            self.tree.delete(item)
        
        if self.dts_root:
            # 插入一个代表虚拟根节点的项，用户可以选中它
            # 这个项的text可以设置为"/"或者"DTS Root"
            self.tree.insert("", tk.END, iid=self.VIRTUAL_ROOT_ID, text="/", values=('节点',), open=True)

            # 加载直接位于虚拟根节点 (表示 DTS 文件顶层 '/' 节点) 下的属性
            for prop_name, prop_value in self.dts_root.properties.items():
                display_value = prop_value.strip('<>"') # 显示时去除引号和尖括号
                # 插入到虚拟根节点的子节点中
                self.tree.insert(self.VIRTUAL_ROOT_ID, tk.END, text=f"{prop_name} = {display_value}", values=('属性',))

            # 填充根节点的子节点
            for child in self.dts_root.children.values():
                self._populate_node(child, self.VIRTUAL_ROOT_ID) # 将顶级节点作为虚拟根节点的子节点
    
    def _populate_node(self, node: DTSNode, parent_id: str):
        """递归填充节点"""
        # 插入节点，open=False 使得节点默认折叠
        node_id = self.tree.insert(parent_id, tk.END, text=node.name, values=('节点',), open=False) 
        
        # 插入属性
        for prop_name, prop_value in node.properties.items():
            # 在树状图中显示时去除引号和尖括号
            display_value = prop_value.strip('<>"')
            self.tree.insert(node_id, tk.END, text=f"{prop_name} = {display_value}", values=('属性',))
        
        # 递归插入子节点
        for child in node.children.values():
            self._populate_node(child, node_id)
    
    def on_tree_select(self, event):
        """树形视图选择事件"""
        selection = self.tree.selection()
        if not selection:
            self.path_var.set("") # 清空路径显示
            self.populate_property_tree(None) # 清空属性面板
            return
        
        item = selection[0]
        
        if item == self.VIRTUAL_ROOT_ID:
            node = self.dts_root
        else:
            # 判断选中项是节点还是属性
            values = self.tree.item(item, 'values')
            if values and values[0] == '属性':
                # 如果选中是属性，则找到其父节点
                parent_item = self.tree.parent(item)
                if parent_item == self.VIRTUAL_ROOT_ID: # 根节点属性的父item是虚拟根节点ID
                    node = self.dts_root
                else:
                    node = self._get_node_from_item(parent_item)
            else: # 选中是节点
                node = self._get_node_from_item(item)
        
        if node:
            # 对于虚拟根节点，其 full_path 为空，显示为 "/"
            display_path = node.full_path if node.full_path != "" else "/"
            self.path_var.set(display_path)
            self.populate_property_tree(node)
        else:
            self.path_var.set("")
            # 清空属性面板
            self.populate_property_tree(None)

    
    def _get_node_from_item(self, item) -> Optional[DTSNode]:
        """从树形视图项获取节点"""
        if item == self.VIRTUAL_ROOT_ID:
            return self.dts_root

        # 从选中的项向上追溯，构建路径
        path_names = []
        current_item = item
        
        while current_item and current_item != self.VIRTUAL_ROOT_ID:
            values = self.tree.item(current_item, 'values')
            # 只有是节点类型才加入路径
            if values and values[0] == '节点': 
                path_names.append(self.tree.item(current_item, 'text'))
            current_item = self.tree.parent(current_item)
        
        path_names.reverse() # 路径是从根到当前节点
        
        # 如果 path_names 是空的，说明选中的是顶层（虚拟根节点）下的属性，或者就是虚拟根节点
        if not path_names:
            # 这种情况应该由 on_tree_select 中对 VIRTUAL_ROOT_ID 的判断来处理
            return None 
        
        # 从dts_root开始查找
        current_dts_node = self.dts_root
        # 忽略第一个虚拟的'root'节点，因为 path_names 已经包含了实际的顶层DTS节点名
        for name in path_names:
            if name in current_dts_node.children:
                current_dts_node = current_dts_node.children[name]
            else:
                return None # 路径不匹配，节点不存在
        return current_dts_node
    
    def _find_node_by_path(self, path_list: List[str]) -> Optional[DTSNode]:
        """根据路径列表查找节点"""
        current = self.dts_root
        
        # 路径列表应该从顶级节点开始，例如 ["soc", "sub_node"]
        for name in path_list:
            if name in current.children:
                current = current.children[name]
            else:
                return None
        
        return current
    
    def populate_property_tree(self, node: Optional[DTSNode]):
        """填充属性树"""
        # 清空属性树
        for item in self.prop_tree.get_children():
            self.prop_tree.delete(item)
        
        if node:
            # 添加属性
            for prop_name, prop_value in node.properties.items():
                # 在属性面板中显示时去除引号和尖括号
                display_value = prop_value.strip('<>"')
                self.prop_tree.insert("", tk.END, text=prop_name, values=(display_value,))
    
    def on_tree_double_click(self, event):
        """树形视图双击事件"""
        item = self.tree.selection()[0]
        values = self.tree.item(item, 'values')
        
        if values and values[0] == '属性':
            # 双击属性，编辑属性值
            # prop_name是从treeview的text中解析出来的，text格式为 "prop_name = display_value"
            full_text = self.tree.item(item, 'text')
            if ' = ' in full_text:
                prop_name = full_text.split(' = ', 1)[0] # 提取属性名
                
                parent_item = self.tree.parent(item)
                
                # 处理根节点属性的情况
                if parent_item == self.VIRTUAL_ROOT_ID: # 根节点属性的父item是虚拟根节点ID
                    node = self.dts_root
                else:
                    node = self._get_node_from_item(parent_item)
                
                if node and prop_name in node.properties:
                    original_value_from_node = node.properties.get(prop_name, "")
                    
                    # 判断原始值的格式
                    value_format_type = "plain" # 默认是普通值
                    if original_value_from_node.startswith('"') and original_value_from_node.endswith('"'):
                        value_format_type = "string"
                    elif original_value_from_node.startswith('<') and original_value_from_node.endswith('>'):
                        value_format_type = "hex_array"

                    # 弹出对话框时去除引号和尖括号
                    initial_display_value = original_value_from_node.strip('<>"') 

                    # 使用自定义对话框 PropertyEditDialog
                    dialog = PropertyEditDialog(self.root, "编辑属性", f"编辑属性 '{prop_name}' 的值:", initialvalue=initial_display_value)
                    new_value = dialog.result
                    
                    if new_value is not None:
                        # 在保存时根据原始格式添加回去
                        if value_format_type == "string" and not (new_value.startswith('"') and new_value.endswith('"')):
                            formatted_new_value = f'"{new_value}"'
                        elif value_format_type == "hex_array" and not (new_value.startswith('<') and new_value.endswith('>')):
                            formatted_new_value = f'<{new_value}>'
                        else:
                            formatted_new_value = new_value # 用户输入已经包含格式或原始就是普通值

                        node.set_property(prop_name, formatted_new_value)
                        
                        # 选中节点可能已经改变，需要重新获取
                        selected_node_item = self.tree.selection()[0] if self.tree.selection() else None
                        if selected_node_item:
                            # 重新选择以确保属性面板更新
                            if parent_item == self.VIRTUAL_ROOT_ID: # 如果是根节点属性，刷新根节点
                                self.populate_property_tree(self.dts_root)
                            else:
                                selected_node = self._get_node_from_item(selected_node_item)
                                if selected_node: # 确保选中的还是当前节点
                                    self.populate_property_tree(selected_node) # 更新属性面板
                        self.populate_tree() # 刷新主树，显示更新
                else:
                    messagebox.showwarning("警告", "无法找到对应的节点或属性")
    
    def edit_property_value(self, event):
        """编辑属性值 (通过属性面板双击)"""
        selection = self.prop_tree.selection()
        if not selection:
            return
        
        item = selection[0]
        prop_name = self.prop_tree.item(item, 'text')
        
        # 获取当前选中的节点 (主树中选中的节点)
        tree_selection = self.tree.selection()
        if not tree_selection:
            messagebox.showwarning("警告", "请先选择一个节点或根节点下的属性")
            return
        
        selected_tree_item = tree_selection[0]
        
        node = None
        if selected_tree_item == self.VIRTUAL_ROOT_ID:
            node = self.dts_root
        else:
            selected_item_values = self.tree.item(selected_tree_item, 'values')
            if selected_item_values and selected_item_values[0] == '属性':
                # 如果选择的是根节点下的属性
                parent_of_selected_prop = self.tree.parent(selected_tree_item)
                if parent_of_selected_prop == self.VIRTUAL_ROOT_ID: # 根节点下的属性
                    node = self.dts_root
                else: # 普通节点下的属性
                    node = self._get_node_from_item(parent_of_selected_prop)
            else: # 选择的是节点
                node = self._get_node_from_item(selected_tree_item)

        if not node:
            messagebox.showwarning("警告", "请选择一个有效的节点或根节点下的属性")
            return

        original_value_from_node = node.properties.get(prop_name, "")
        
        # 判断原始值的格式
        value_format_type = "plain" # 默认是普通值
        if original_value_from_node.startswith('"') and original_value_from_node.endswith('"'):
            value_format_type = "string"
        elif original_value_from_node.startswith('<') and original_value_from_node.endswith('>'):
            value_format_type = "hex_array"

        # 弹出对话框时去除引号和尖括号
        initial_display_value = original_value_from_node.strip('<>"') 
        
        # 使用自定义对话框 PropertyEditDialog
        dialog = PropertyEditDialog(self.root, "编辑属性", f"编辑属性 '{prop_name}' 的值:", initialvalue=initial_display_value)
        new_value = dialog.result

        if new_value is not None:
            # 在保存时根据原始格式添加回去
            if value_format_type == "string" and not (new_value.startswith('"') and new_value.endswith('"')):
                formatted_new_value = f'"{new_value}"'
            elif value_format_type == "hex_array" and not (new_value.startswith('<') and new_value.endswith('>')):
                formatted_new_value = f'<{new_value}>'
            else:
                formatted_new_value = new_value # 用户输入已经包含格式或原始就是普通值

            node.set_property(prop_name, formatted_new_value)
            self.populate_property_tree(node)
            self.populate_tree()
    
    def add_node(self):
        """添加节点"""
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("警告", "请先选择一个节点作为父节点")
            return
        
        item = selection[0]
        # 如果选中项是虚拟根节点ID，则父节点是dts_root
        if item == self.VIRTUAL_ROOT_ID:
            parent_node = self.dts_root
        else:
            item_type = self.tree.item(item, 'values')[0]
            if item_type == '属性':
                messagebox.showwarning("警告", "不能在属性下添加节点，请选择一个节点作为父节点。")
                return
            parent_node = self._get_node_from_item(item)
        
        if not parent_node:
            messagebox.showwarning("警告", "请选择一个有效的节点")
            return
        
        node_name = simpledialog.askstring("添加节点", "请输入节点名称:")
        if node_name:
            # 检查节点名称是否有效
            # 允许 DTS 节点名包含字母、数字、下划线、连字符、逗号、点，以及 "@" 和 "#" (例如 some@address, #address-cells)
            # 等号 "=" 通常不会出现在节点名中，而是用于属性赋值
            if not re.fullmatch(r'[a-zA-Z0-9_\-,\.#@]+', node_name): 
                messagebox.showerror("错误", "节点名称包含非法字符，只能包含字母、数字、下划线、连字符、逗号、点、井号或@符号。")
                return

            if node_name in parent_node.children:
                messagebox.showwarning("警告", f"节点 '{node_name}' 已存在于当前父节点下。")
                return
            new_node = DTSNode(node_name, parent_node) # 传入parent
            parent_node.add_child(new_node)
            self.populate_tree()
            messagebox.showinfo("成功", f"节点 '{node_name}' 添加成功")
    
    def delete_node(self):
        """删除节点"""
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("警告", "请先选择要删除的节点")
            return
        
        item = selection[0]
        
        if item == self.VIRTUAL_ROOT_ID:
            messagebox.showwarning("警告", "无法删除DTS根节点。")
            return

        values = self.tree.item(item, 'values')
        
        if not values or values[0] != '节点':
            messagebox.showwarning("警告", "请选择一个节点进行删除")
            return
        
        node = self._get_node_from_item(item)
        
        if node is None:
            messagebox.showerror("错误", "无法找到要删除的节点。")
            return

        # 检查是否是虚拟根节点下的直接子节点
        is_top_level_dts_node = (node.parent and node.parent.name == "root")
        
        if is_top_level_dts_node:
            # 如果是顶级DTS节点 (例如 /soc)，则给出更明确的警告
            if not messagebox.askyesno("确认", f"确定要删除顶级DTS节点 '{node.name}' 及其所有子节点吗？这将影响整个DTS结构。"):
                return
        else:
            if not messagebox.askyesno("确认", f"确定要删除节点 '{node.name}' ({node.full_path}) 及其所有子节点吗?"):
                return
        
        if node.parent: # 根节点没有parent，dts_root是虚拟的
            node.parent.remove_child(node.name)
            self.populate_tree()
            # 清空属性面板，因为当前选中的节点可能被删除了
            for item in self.prop_tree.get_children():
                self.prop_tree.delete(item)
            self.path_var.set("")
            messagebox.showinfo("成功", f"节点 '{node.name}' 删除成功")
        else:
            messagebox.showerror("错误", "无法删除该节点。") # 这通常不会发生，除非尝试删除虚拟根节点
    
    def add_property(self):
        """添加属性"""
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("警告", "请先选择一个节点")
            return
        
        item = selection[0]
        # 判断选中项是节点还是属性，如果是属性则找父节点
        if item == self.VIRTUAL_ROOT_ID:
            node = self.dts_root
        else:
            values = self.tree.item(item, 'values')
            if values and values[0] == '属性':
                parent_item = self.tree.parent(item)
                if parent_item == self.VIRTUAL_ROOT_ID: # 根节点属性
                    node = self.dts_root
                else:
                    node = self._get_node_from_item(parent_item)
            else: # 选中是节点
                node = self._get_node_from_item(item)

        if not node:
            messagebox.showwarning("警告", "请选择一个有效的节点")
            return
        
        # 将属性名和值放在一个弹窗中
        prop_dialog = simpledialog.askstring("添加属性", "请输入属性名称和值 (格式: 属性名=值):")
        if prop_dialog:
            if '=' not in prop_dialog:
                messagebox.showerror("错误", "输入格式不正确，应为 '属性名=值'")
                return

            prop_name, prop_value = prop_dialog.split('=', 1)
            prop_name = prop_name.strip()
            prop_value = prop_value.strip()

            # 检查属性名称是否有效，允许DTS属性名包含字母、数字、下划线、连字符、逗号、点、井号、@符号
            if not re.fullmatch(r'[a-zA-Z0-9_\-,\.#@]+', prop_name): 
                messagebox.showerror("错误", "属性名称包含非法字符，只能包含字母、数字、下划线、连字符、逗号、点、井号或@符号。")
                return

            if prop_name in node.properties:
                messagebox.showwarning("警告", f"属性 '{prop_name}' 已存在于当前节点下。")
                return
            
            # 这里不自动添加引号或尖括号，用户需要手动输入
            node.set_property(prop_name, prop_value)
            self.populate_property_tree(node)
            self.populate_tree()
            messagebox.showinfo("成功", f"属性 '{prop_name}' 添加成功")
    
    def delete_property(self):
        """删除属性"""
        selection = self.prop_tree.selection()
        if not selection:
            messagebox.showwarning("警告", "请先选择要删除的属性")
            return
        
        item = selection[0]
        prop_name = self.prop_tree.item(item, 'text') # 从text获取属性名
        
        # 获取当前选中的节点 (不是属性面板中选中的属性，而是主树中选中的节点)
        tree_selection = self.tree.selection()
        if not tree_selection:
            messagebox.showwarning("警告", "请先选择一个节点")
            return
        
        selected_tree_item = tree_selection[0]

        node = None
        if selected_tree_item == self.VIRTUAL_ROOT_ID:
            node = self.dts_root
        else:
            selected_item_values = self.tree.item(selected_tree_item, 'values')
            if selected_item_values and selected_item_values[0] == '属性':
                # 如果主树中选中的是属性（意味着属性面板显示的是这个属性的父节点）
                parent_of_selected_prop = self.tree.parent(selected_tree_item)
                if parent_of_selected_prop == self.VIRTUAL_ROOT_ID: # 根节点属性
                    node = self.dts_root
                else:
                    node = self._get_node_from_item(parent_of_selected_prop)
            else: # 主树中选中是节点
                node = self._get_node_from_item(selected_tree_item)

        if not node:
            messagebox.showwarning("警告", "请选择一个有效的节点")
            return
        
        if messagebox.askyesno("确认", f"确定要删除属性 '{prop_name}' 吗?"):
            node.remove_property(prop_name)
            self.populate_property_tree(node)
            self.populate_tree()
            messagebox.showinfo("成功", f"属性 '{prop_name}' 删除成功")
    
    def search_nodes(self):
        """搜索节点"""
        search_text = self.search_var.get().strip()
        if not search_text:
            messagebox.showwarning("警告", "请输入搜索内容")
            return
        
        results = []
        # 从dts_root的子节点开始搜索，因为dts_root本身是虚拟的
        # 也搜索根节点本身
        if search_text.lower() in self.dts_root.name.lower() or search_text.lower() == "/":
             results.append(self.dts_root)

        for child in self.dts_root.children.values():
            self._search_nodes_recursive(child, search_text, results)
        
        if results:
            result_text = "\n".join([f"节点: {node.full_path if node.full_path != '' else '/'}" for node in results])
            messagebox.showinfo("搜索结果", f"找到 {len(results)} 个节点:\n{result_text}")
            # 选中第一个搜索结果
            if results:
                self._select_node_in_tree(results[0])
        else:
            messagebox.showinfo("搜索结果", "未找到匹配的节点")
    
    def _search_nodes_recursive(self, node: DTSNode, search_text: str, results: List[DTSNode]):
        """递归搜索节点"""
        if search_text.lower() in node.name.lower():
            results.append(node)
        
        for child in node.children.values():
            self._search_nodes_recursive(child, search_text, results)
    
    def search_properties(self):
        """搜索属性"""
        search_text = self.search_var.get().strip()
        if not search_text:
            messagebox.showwarning("警告", "请输入搜索内容")
            return
        
        results = []
        # 搜索根节点自身的属性
        for prop_name, prop_value in self.dts_root.properties.items():
            if (search_text.lower() in prop_name.lower() or 
                search_text.lower() in prop_value.lower()):
                # 对于根节点的属性，我们将其所属节点视为虚拟根节点
                results.append((self.dts_root, prop_name, prop_value))

        # 从dts_root的子节点开始搜索
        for child in self.dts_root.children.values():
            self._search_properties_recursive(child, search_text, results)
        
        if results:
            # 在显示结果时，对属性值去除引号和尖括号
            result_text = "\n".join([f"{node.get_property_path(prop)}: {value.strip('<>\"')}" for node, prop, value in results]) 
            messagebox.showinfo("搜索结果", f"找到 {len(results)} 个属性:\n{result_text}")
            # 选中第一个搜索结果的节点
            if results:
                self._select_node_in_tree(results[0][0], results[0][1]) # 选中属性所属的节点，并尝试选中属性
        else:
            messagebox.showinfo("搜索结果", "未找到匹配的属性")
    
    def _search_properties_recursive(self, node: DTSNode, search_text: str, results: List[Tuple]):
        """递归搜索属性"""
        for prop_name, prop_value in node.properties.items():
            if (search_text.lower() in prop_name.lower() or 
                search_text.lower() in prop_value.lower()): # 搜索属性名或属性值
                results.append((node, prop_name, prop_value))
        
        for child in node.children.values():
            self._search_properties_recursive(child, search_text, results)
            
    def _select_node_in_tree(self, target_node: DTSNode, target_prop_name: Optional[str] = None):
        """在树状图中选中并展开指定节点，如果指定了属性，则尝试选中属性"""
        # 清除当前选择
        self.tree.selection_remove(self.tree.selection())

        # 如果目标是虚拟根节点
        if target_node == self.dts_root:
            if target_prop_name:
                # 遍历虚拟根节点的子项，查找属性
                for item in self.tree.get_children(self.VIRTUAL_ROOT_ID):
                    if self.tree.item(item, 'values') and self.tree.item(item, 'values')[0] == '属性':
                        full_text = self.tree.item(item, 'text')
                        if ' = ' in full_text:
                            prop_name_in_tree = full_text.split(' = ', 1)[0]
                            if prop_name_in_tree == target_prop_name:
                                self.tree.selection_set(item)
                                self.tree.focus(item)
                                self.tree.see(item)
                                return
            else:
                self.tree.selection_set(self.VIRTUAL_ROOT_ID)
                self.tree.focus(self.VIRTUAL_ROOT_ID)
                self.tree.see(self.VIRTUAL_ROOT_ID)
            return

        # 构建目标节点的完整路径名列表 (不包括虚拟根节点)
        path_names = []
        current = target_node
        # 向上追溯，直到虚拟根节点
        while current and current.parent and current.parent.name != "root":
            path_names.insert(0, current.name)
            current = current.parent
        # 如果是虚拟根节点的直接子节点 (例如 /soc)
        if current and current.parent and current.parent.name == "root" and current.name != "root":
            path_names.insert(0, current.name)

        current_tree_item = self.VIRTUAL_ROOT_ID # 从虚拟根节点开始查找
        
        for name_part in path_names:
            found_child_item = None
            for child_item in self.tree.get_children(current_tree_item):
                # 确保是节点类型且名称匹配
                if self.tree.item(child_item, 'text') == name_part and self.tree.item(child_item, 'values')[0] == '节点':
                    found_child_item = child_item
                    break
            if found_child_item:
                self.tree.item(found_child_item, open=True) # 展开当前节点
                current_tree_item = found_child_item
            else:
                # 节点在树状图中可能尚未渲染或路径不匹配
                return
        
        if current_tree_item:
            if target_prop_name:
                # 选中指定节点的属性
                for child_item in self.tree.get_children(current_tree_item):
                    if self.tree.item(child_item, 'values') and self.tree.item(child_item, 'values')[0] == '属性':
                        full_text = self.tree.item(child_item, 'text')
                        if ' = ' in full_text:
                            prop_name_in_tree = full_text.split(' = ', 1)[0]
                            if prop_name_in_tree == target_prop_name:
                                self.tree.selection_set(child_item) # 选中目标属性
                                self.tree.focus(child_item) # 聚焦到目标属性
                                self.tree.see(child_item) # 滚动到可见区域
                                return # 选中属性后返回
            
            # 如果没有指定属性，或者属性未找到，则选中节点本身
            self.tree.selection_set(current_tree_item) # 选中目标节点
            self.tree.focus(current_tree_item) # 聚焦到目标节点
            self.tree.see(current_tree_item) # 滚动到可见区域
    
    def save_as_dts(self):
        """保存为DTS文件"""
        if not self.dts_root:
            messagebox.showwarning("警告", "没有可保存的内容")
            return
        
        file_path = filedialog.asksaveasfilename(
            title="保存DTS文件",
            defaultextension=".dts",
            filetypes=[("DTS files", "*.dts"), ("All files", "*.*")]
        )
        
        if file_path:
            try:
                content = ""
                # DTS文件的最外层通常是一个匿名根节点或以 '/' 命名的根节点
                # 这里强制输出为 / { ... }; 结构，以包含所有顶层属性和节点
                content += "/ {\n"
                # 输出根节点自身的属性
                for prop_name, prop_value in self.dts_root.properties.items():
                    content += f"    {prop_name} = {prop_value};\n"
                
                # 输出顶级子节点
                for child in self.dts_root.children.values():
                    content += self._generate_dts_content(child, 1) # 顶级子节点缩进一层
                content += "};\n"

                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(content)
                messagebox.showinfo("成功", f"DTS文件保存成功: {file_path}")
            except Exception as e:
                messagebox.showerror("错误", f"保存DTS文件失败: {e}")
    
    def _generate_dts_content(self, node: DTSNode, indent: int = 0) -> str:
        """生成DTS内容"""
        content = ""
        indent_str = "    " * indent
        
        # 递归生成节点内容
        content += f"{indent_str}{node.name} {{\n"
        
        # 输出属性
        for prop_name, prop_value in node.properties.items():
            content += f"{indent_str}    {prop_name} = {prop_value};\n"
        
        # 输出子节点
        for child in node.children.values():
            content += self._generate_dts_content(child, indent + 1)
        
        content += f"{indent_str}}};\n"
        
        return content
    
    def generate_patch(self):
        """生成补丁文件"""
        if not self.dts_root or not self.original_dts_root:
            messagebox.showwarning("警告", "请先加载DTS文件才能生成补丁")
            return
        
        file_path = filedialog.asksaveasfilename(
            title="保存补丁文件",
            defaultextension=".patch",
            filetypes=[("Patch files", "*.patch"), ("All files", "*.*")]
        )
        
        if file_path:
            try:
                patch_content_lines = self._get_patch_lines(self.dts_root, self.original_dts_root)
                
                # 确保最后有一个空行
                patch_content = "\n".join(patch_content_lines) + "\n"
                
                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(patch_content)
                messagebox.showinfo("成功", f"补丁文件生成成功: {file_path}\n")
            except Exception as e:
                messagebox.showerror("错误", f"生成补丁文件失败: {e}")

    def generate_diff_patch(self):
        """生成两个DTS文件的差异补丁"""
        messagebox.showinfo("选择DTS文件", "请先选择基准DTS文件 (DTS1)。")
        file_path1 = filedialog.askopenfilename(
            title="选择基准DTS文件 (DTS1)",
            filetypes=[("DTS files", "*.dts"), ("All files", "*.*")]
        )
        if not file_path1:
            return

        messagebox.showinfo("选择DTS文件", "请选择目标DTS文件 (DTS2)。")
        file_path2 = filedialog.askopenfilename(
            title="选择目标DTS文件 (DTS2)",
            filetypes=[("DTS files", "*.dts"), ("All files", "*.*")]
        )
        if not file_path2:
            return

        try:
            parser1 = DTSParser()
            dts1_root = parser1.parse_file(file_path1)

            parser2 = DTSParser()
            dts2_root = parser2.parse_file(file_path2)
            
            patch_content_lines = self._get_patch_lines(dts2_root, dts1_root)

            # 保存补丁
            save_path = filedialog.asksaveasfilename(
                title="保存差异补丁文件",
                defaultextension=".patch",
                filetypes=[("Patch files", "*.patch"), ("All files", "*.*")]
            )
            if save_path:
                patch_content = "\n".join(patch_content_lines) + "\n"
                with open(save_path, 'w', encoding='utf-8') as f:
                    f.write(patch_content)
                messagebox.showinfo("成功", f"差异补丁文件生成成功: {save_path}")

        except Exception as e:
            messagebox.showerror("错误", f"生成差异补丁失败: {e}")

    def _get_patch_lines(self, current_dts_root: DTSNode, original_dts_root: DTSNode) -> List[str]:
        """
        生成补丁行的核心逻辑，返回一个包含补丁行的列表。
        按照 d (删除属性), r (删除节点), c (创建节点), a (创建属性), 修改值 的顺序排序。
        """
        patch_lines_delete_prop = []
        patch_lines_remove_node = []
        patch_lines_create_node = []
        patch_lines_create_prop = []
        patch_lines_modify_prop = []

        # 辅助函数，收集差异
        def collect_diffs_recursive(curr_node: DTSNode, orig_node: DTSNode):
            # 1. 比较属性
            # 查找被删除的属性 (在原始节点有，在当前节点无)
            for prop_name in orig_node.properties:
                if prop_name not in curr_node.properties:
                    patch_lines_delete_prop.append(f"d {orig_node.get_property_path(prop_name)}")
            
            # 查找新增或修改的属性
            for prop_name, curr_prop_value in curr_node.properties.items():
                orig_prop_value = orig_node.properties.get(prop_name)
                
                # Bug1 Fix: 生成补丁时不应该带””和<>，所以这里进行strip
                # 这里对比的时候需要使用strip后的值进行对比
                curr_stripped_value = str(curr_prop_value).strip('<>"')
                orig_stripped_value = str(orig_prop_value).strip('<>"') if orig_prop_value is not None else None

                if orig_prop_value is None:
                    # 新增属性
                    patch_lines_create_prop.append(f"a {curr_node.get_property_path(prop_name)} {curr_stripped_value}")
                elif orig_stripped_value != curr_stripped_value: # 比较去除引号和尖括号后的值
                    # 属性值被修改
                    patch_lines_modify_prop.append(f"{curr_node.get_property_path(prop_name)} {curr_stripped_value}")

            # 2. 比较子节点
            # 查找被删除的节点 (在原始树有，在当前树无)
            for orig_child_name, orig_child_node in orig_node.children.items():
                if orig_child_name not in curr_node.children:
                    # 确保路径以 / 开头，并且是相对于顶级DTS节点的完整路径
                    patch_lines_remove_node.append(f"r {orig_child_node.full_path}")
            
            # 查找新增或修改的节点
            for curr_child_name, curr_child_node in curr_node.children.items():
                if curr_child_name not in orig_node.children:
                    # 新增节点
                    patch_lines_create_node.append(f"c {curr_child_node.full_path}")
                    # 新增节点的所有属性 (以 'a' 开头创建)
                    # 递归收集新增节点下的所有属性
                    def collect_new_node_props(node: DTSNode):
                        for prop_name, prop_value in node.properties.items():
                            patch_lines_create_prop.append(f"a {node.get_property_path(prop_name)} {str(prop_value).strip('<>\"')}")
                        for child in node.children.values():
                            collect_new_node_props(child)
                    collect_new_node_props(curr_child_node)

                else:
                    # 节点存在于两棵树中，递归比较其内部
                    orig_child_node = orig_node.children[curr_child_name]
                    collect_diffs_recursive(curr_child_node, orig_child_node) # 递归比较子节点

        # 从根节点开始收集差异
        collect_diffs_recursive(current_dts_root, original_dts_root)

        # 按照 d, r, c, a, 修改值 的顺序拼接补丁行
        all_patch_lines = []
        all_patch_lines.extend(patch_lines_delete_prop)
        all_patch_lines.extend(patch_lines_remove_node)
        all_patch_lines.extend(patch_lines_create_node)
        all_patch_lines.extend(patch_lines_create_prop)
        all_patch_lines.extend(patch_lines_modify_prop)

        return all_patch_lines
    
    def load_patch(self):
        """加载补丁文件"""
        if not self.dts_root:
            messagebox.showwarning("警告", "请先打开一个DTS文件才能加载补丁")
            return

        file_path = filedialog.askopenfilename(
            title="选择补丁文件",
            filetypes=[("Patch files", "*.patch"), ("All files", "*.*")]
        )
        
        if file_path:
            try:
                with open(file_path, 'r', encoding='utf-8') as f:
                    patch_content = f.read()
                
                self._apply_patch(patch_content)
                self.populate_tree() # 重新填充树状图以显示更改
                # 更新original_dts_root，使其反映应用补丁后的状态
                self.original_dts_root = self._deep_copy_dts_node(self.dts_root) 
                messagebox.showinfo("成功", "补丁加载成功!")
            except Exception as e:
                messagebox.showerror("错误", f"加载补丁失败: {e}")
    
    def _apply_patch(self, patch_content: str):
        """应用补丁"""
        lines = patch_content.strip().split('\n')
        
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'): # 忽略空行和注释行 (如果存在)
                continue
            
            if line.startswith('r '):
                # 删除节点
                node_path = line[2:].strip()
                self._remove_node_by_path(node_path)
            elif line.startswith('d '):
                # 删除属性
                attr_path = line[2:].strip()
                self._remove_property_by_path(attr_path)
            elif line.startswith('c '):
                # 创建节点
                node_path = line[2:].strip()
                self._create_node_by_path(node_path)
            elif line.startswith('a '):
                # 创建属性
                parts = line[2:].strip().split(' ', 1)
                if len(parts) == 2:
                    attr_path, value = parts
                    # Bug1 Fix: 应用补丁时，需要根据值的内容判断是否添加引号或尖括号
                    # 如果值包含空格，或者以引号/尖括号开头，则保留原样
                    # 否则如果需要作为字符串或十六进制数组，则根据实际情况添加
                    formatted_value = self._format_value_for_dts(value)
                    self._create_property_by_path(attr_path, formatted_value)
            else:
                # 修改属性值
                parts = line.split(' ', 1)
                if len(parts) == 2:
                    attr_path, value = parts
                    # Bug1 Fix: 应用补丁时，需要根据值的内容判断是否添加引号或尖括号
                    formatted_value = self._format_value_for_dts(value)
                    self._modify_property_by_path(attr_path, formatted_value)
                else:
                    print(f"警告：无法解析的补丁行: {line}")
    
    def _format_value_for_dts(self, value: str) -> str:
        """根据补丁中的值判断是否需要添加引号或尖括号以符合DTS格式"""
        # 如果值已经包含引号或尖括号，则直接使用
        if (value.startswith('"') and value.endswith('"')) or \
           (value.startswith('<') and value.endswith('>')):
            return value
        
        # 尝试判断是否是十六进制数组 (包含 '0x' 且只包含十六进制字符和空格)
        # 注意这里需要更严格的判断，避免将普通字符串误判为十六进制数组
        if re.fullmatch(r'^(0x[0-9a-fA-F]+\s*)+$', value.strip()):
            return f"<{value}>"
        
        # 如果值中包含空格，并且不是十六进制数组，则视为字符串
        if ' ' in value and not re.fullmatch(r'^(0x[0-9a-fA-F]+\s*)+$', value.strip()):
            return f'"{value}"'

        # 否则，视为普通值，不加引号或尖括号
        return value


    def _get_node_from_path_string(self, path_string: str) -> Optional[DTSNode]:
        """根据路径字符串获取节点，处理虚拟根节点"""
        # 移除开头的 "/"，然后按 "/" 分割
        parts = path_string.strip('/').split('/')
        
        if not parts or (len(parts) == 1 and parts[0] == ''): # 路径是 "/" 或空
            return self.dts_root # 代表虚拟根节点
        
        current_node = self.dts_root
        for part in parts:
            if part in current_node.children:
                current_node = current_node.children[part]
            else:
                return None # 路径中途不存在
        return current_node

    def _remove_node_by_path(self, node_path: str):
        """根据路径删除节点"""
        # 移除开头的 "/"，然后按 "/" 分割
        parts = node_path.strip('/').split('/')
        if not parts or (len(parts) == 1 and parts[0] == ''): # 路径是 "/" 或空，不能删除虚拟根节点
            print(f"警告：无法删除虚拟根节点: {node_path}")
            return
        
        # 查找父节点
        parent_path_parts = parts[:-1]
        node_name_to_remove = parts[-1]

        parent_node = self._get_node_from_path_string('/'.join(parent_path_parts) if parent_path_parts else "/")
        
        if parent_node:
            if node_name_to_remove in parent_node.children:
                parent_node.remove_child(node_name_to_remove)
            else:
                print(f"警告：尝试删除不存在的节点: '{node_name_to_remove}' (路径: '{node_path}')")
        else:
            print(f"警告：删除节点时，路径中找不到父节点 (完整路径: '{node_path}')")
    
    def _remove_property_by_path(self, attr_path: str):
        """根据路径删除属性"""
        # 移除开头的 "/"，然后按 "/" 分割
        parts = attr_path.strip('/').split('/')
        
        if not parts or (len(parts) == 1 and parts[0] == ''): # 路径是 "/" 或空
             print(f"警告：无效的属性删除路径 (路径太短或指向根): {attr_path}")
             return
        
        prop_name_to_remove = parts[-1]
        node_path_parts = parts[:-1]

        # 如果 node_path_parts 为空，表示是根节点的属性，例如 "/author"
        if not node_path_parts: 
            node = self.dts_root
        else:
            node = self._get_node_from_path_string('/'.join(node_path_parts))
        
        if node:
            if prop_name_to_remove in node.properties:
                node.remove_property(prop_name_to_remove)
            else:
                print(f"警告：尝试删除不存在的属性: '{prop_name_to_remove}' (路径: '{attr_path}')")
        else:
            print(f"警告：删除属性时，路径中找不到节点 (完整路径: '{attr_path}')")
    
    def _create_node_by_path(self, node_path: str):
        """根据路径创建节点"""
        # 移除开头的 "/"，然后按 "/" 分割
        parts = node_path.strip('/').split('/')
        if not parts or (len(parts) == 1 and parts[0] == ''): # 路径是 "/" 或空
            print(f"警告：无效的节点创建路径 (路径太短或指向根): {node_path}")
            return
        
        current_node = self.dts_root # 从虚拟根节点开始
        for part in parts: # 遍历所有路径部分，包括要创建的节点名
            if part not in current_node.children:
                new_node = DTSNode(part, current_node) # 传入父节点
                current_node.add_child(new_node)
            current_node = current_node.children[part] # 移动到子节点
    
    def _create_property_by_path(self, attr_path: str, value: str):
        """根据路径创建属性"""
        # 移除开头的 "/"，然后按 "/" 分割
        parts = attr_path.strip('/').split('/')
        
        if not parts or (len(parts) == 1 and parts[0] == ''): # 路径是 "/" 或空
            print(f"警告：无效的属性创建路径 (路径太短或指向根): {attr_path}")
            return
        
        prop_name_to_create = parts[-1]
        node_path_parts = parts[:-1]

        # 找到或创建节点
        current_node = self.dts_root
        # 如果 node_path_parts 为空，表示属性直接添加到根节点
        if not node_path_parts: 
            node_to_add_prop = self.dts_root
        else:
            node_to_add_prop = self.dts_root
            for part in node_path_parts:
                if part not in node_to_add_prop.children:
                    new_node = DTSNode(part, node_to_add_prop)
                    node_to_add_prop.add_child(new_node)
                node_to_add_prop = node_to_add_prop.children[part]
        
        # 创建属性 (值直接使用补丁中的值，不进行 strip)
        node_to_add_prop.set_property(prop_name_to_create, value)
    
    def _modify_property_by_path(self, attr_path: str, value: str):
        """根据路径修改属性值"""
        # 移除开头的 "/"，然后按 "/" 分割
        parts = attr_path.strip('/').split('/')
        
        if not parts or (len(parts) == 1 and parts[0] == ''): # 路径是 "/" 或空
            print(f"警告：无效的属性修改路径 (路径太短或指向根): {attr_path}")
            return
        
        prop_name_to_modify = parts[-1]
        node_path_parts = parts[:-1]
        
        # 如果 node_path_parts 为空，表示是根节点的属性
        if not node_path_parts:
            node = self.dts_root
        else:
            node = self._get_node_from_path_string('/'.join(node_path_parts))
        
        if node:
            if prop_name_to_modify in node.properties:
                # 修改属性值 (值直接使用补丁中的值，不进行 strip)
                node.set_property(prop_name_to_modify, value)
            else:
                print(f"警告：修改属性时，属性不存在: '{prop_name_to_modify}' (路径: '{attr_path}')")
        else:
            print(f"警告：修改属性时，路径中找不到节点 (完整路径: '{attr_path}')")


def main():
    """主函数"""
    root = tk.Tk()
    app = DTSEditor(root)
    root.mainloop()


if __name__ == "__main__":
    main()