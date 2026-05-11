import requests
from tqdm import tqdm
import os

def download_file(url, destination):
    response = requests.get(url, stream=True)
    total_size = int(response.headers.get('content-length', 0))
    block_size = 1024 * 1024 # 1MB

    with open(destination, 'wb') as f, tqdm(
        desc=os.path.basename(destination),
        total=total_size,
        unit='iB',
        unit_scale=True,
        unit_divisor=1024,
    ) as bar:
        for data in response.iter_content(block_size):
            size = f.write(data)
            bar.update(size)

if __name__ == "__main__":
    # The URL for the safetensors file of microsoft/bitnet-b1.58-2B-4T-bf16
    url = "https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-bf16/resolve/main/model.safetensors"
    dest_dir = "bitnet_weights/model"
    if not os.path.exists(dest_dir):
        os.makedirs(dest_dir)
    
    dest_path = os.path.join(dest_dir, "model.safetensors")
    print(f"Downloading model.safetensors to {dest_path}...")
    try:
        download_file(url, dest_path)
        print("Download complete!")
    except Exception as e:
        print(f"An error occurred: {e}")
