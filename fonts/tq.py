import os
import re

def extract_strings_from_c_file(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8') as file:
            content = file.read()
    except Exception as e:
        print(f"Error reading {file_path}: {e}")
        return set()

    strings = re.findall(r'\"(.*?)\"', content)
    chinese_characters = set()
    for string in strings:
        chinese_characters.update(re.findall(r'[\u4e00-\u9fff]', string))

    return chinese_characters

def extract_strings_from_directory(directory):
    all_chinese_characters = set()
    for root, dirs, files in os.walk(directory):
        for file in files:
            if file.endswith('.c') or file.endswith('.cpp') or file.endswith('.h') or file.endswith('.inc'):  # assuming you're interested in C source files and headers
                file_path = os.path.join(root, file)
                all_chinese_characters.update(extract_strings_from_c_file(file_path))

    return all_chinese_characters

# 用法示例
directory_path = input('Directory path: ')  # 输入你要遍历的目录路径
extracted_strings = extract_strings_from_directory(directory_path)
print(''.join(sorted(extracted_strings)))
