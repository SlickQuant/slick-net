# vcpkg Integration Guide

This document explains how to use slick-net with vcpkg package manager.

## For Users: Installing slick-net

### vcpkg installation

Once the port is published to the official vcpkg registry:

```bash
vcpkg install slick-net
```

### vcpkg Manifest Mode (Recommended)

Create `vcpkg.json` in your project root:

```json
{
  "name": "my-project",
  "version": "1.0.0",
  "dependencies": [
    "slick-net"
  ]
}
```

## Using slick-net in Your Project

### CMakeLists.txt Configuration

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_project)

set(CMAKE_CXX_STANDARD 20)

# Find slick-net package
find_package(slick-net CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE slick::net)
```

Then configure with CMake:
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```


## For Maintainers: Publishing to vcpkg Registry

### Fork and Clone vcpkg Repository
```bash
git clone https://github.com/YOUR_USERNAME/vcpkg.git
cd vcpkg
```

### Port Files Structure
```
[path to vcpkg]/ports/slick-net/
├── portfile.cmake      # Build instructions
├── vcpkg.json          # Package manifest
└── usage               # Usage instructions for users
```

### Updating the Port

When releasing a new slick-net version (in slick-net repo):

1. Update version in `CMakeLists.txt`
2. Update `CHANGELOG.md`

3. **Rlease the new version in slick-net repository**
   ```bash
   git tag v<new_verson_number>
   git push origin v<new_version_number>
   ```
After the draft release created in https://github.com/SlickQuant/slick-net/releases, click the edit icon then publish the new release.

4. **Calculate SHA512 of Release Archive**
   ```bash
   curl -L https://github.com/SlickQuant/slick-net/archive/refs/tags/v<new_version>.tar.gz | sha512sum
   ```

5. Update SHA512 in `[path to vcpkg]/ports/slick-net/portfile.cmake`
6. Update version in `[path to vcpkg]/ports/slick-net/vcpkg.json`

7. **Add Version Information**
   ```bash
   ./vcpkg x-add-version slick-net
   ```

8. **Test locally**
   ```bash
   ./vcpkg remove slick-net
   ./vcpkg install slick-net
   ```

9. **Submit Pull Request**
   - Create a branch: `git checkout -b update-slick-net-to-<new_version>`
   - Add changed files: `git add *`
   - Commit changes: `git commit -am "[slick-net] update to v<new_version>"`
   - Push: `git push origin update-slick-net-to-<new_version>`
   - Open PR at https://github.com/microsoft/vcpkg

## Troubleshooting

### Port Installation Fails
- Verify SHA512 hash matches the release archive
- Check that all dependencies are available in vcpkg
- Ensure CMake version >= 3.20

### CMake Can't Find slick-net
- Verify vcpkg toolchain file is set: `-DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake`
- Check that slick-net is installed: `vcpkg list | grep slick`
- Ensure you're using `CONFIG` mode: `find_package(slick-net CONFIG REQUIRED)`

## Additional Resources

- [vcpkg Documentation](https://vcpkg.io/)
- [Contributing Ports Guide](https://learn.microsoft.com/en-us/vcpkg/contributing/ports)
- [slick-net Repository](https://github.com/SlickQuant/slick-net)
