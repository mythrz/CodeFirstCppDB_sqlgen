
### ArchLinux EOS

```
sudo pacman -Syu

sudo pacman -S base-devel cmake git bison flex

git clone https://github.com/getml/sqlgen.git
cd sqlgen/

git submodule update --init

cmake -S . -B build -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release

cmake --build build 
```
