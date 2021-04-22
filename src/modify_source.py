import os
import argparse

txt_file = "sources.txt"
MAX_NUM_SOURCES = 4
source_list = ["\n" for n in range(MAX_NUM_SOURCES)]

def add_source(uri):
    print("adding source: ", uri)
    with open(txt_file, "r") as f:
        source_list = f.readlines()
    index = 0
    for s in source_list:
        source_list[index] = s.strip() + "\n"
        if s == "\n":
            source_list[index] = uri.strip()+"\n"
            break
        index += 1
    if (index == len(source_list) and index < MAX_NUM_SOURCES):
        source_list.insert(index, uri.strip()+"\n")
    elif (index == MAX_NUM_SOURCES):
        print("already reached maximum of video streams")
    s = ''.join(source_list)
    with open(txt_file, "w+") as f:
        f.write(s)

def delete_source(id):
    print("deleting source id: ", id)
    with open(txt_file, "r") as f:
        source_list = f.readlines()
    source_list[id] = "\n"
    s = ''.join(source_list)
    with open(txt_file, "w+") as f:
        f.write(s)


if __name__ == "__main__":

    ap = argparse.ArgumentParser()
    ap.add_argument("-a", "--add-source", type=str,
        help="video source addr to add")
    ap.add_argument("-d", "--delete-source", type=int,
        help="video source addr to delete")
    args = vars(ap.parse_args())
    print("--------before---------")
    with open(txt_file, "r") as f:
        source_list = f.readlines()
    print(source_list)

    if args["add_source"] is not None:
        add_source(args["add_source"])
    elif args["delete_source"] is not None:
        delete_source(args["delete_source"])
    else:
        print("nothing happened")

    print("--------after---------")
    with open(txt_file, "r") as f:
        source_list1 = f.readlines()
    print(source_list1)
    
