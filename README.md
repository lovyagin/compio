# Compio

Compio is a lightweight library designed for transparent data compression, enabling efficient and seamless integration into your projects.

## Prerequisites

- **Git**: Required for cloning the repository.
- **CMake**: Used for building the project.
- **vcpkg**: A package manager for managing dependencies.
- A compatible C++ compiler (e.g., GCC for Linux, MSVC for Windows).

## Installation Instructions

### For Linux

1. **Clone the Repository**:

   ```bash
   git clone https://github.com/lovyagin/compio
   cd compio
   ```

2. **Initialize and Update Submodules**:

   ```bash
   git submodule init
   git submodule update
   ```

3. **Set Up vcpkg**:

   ```bash
   ./vcpkg/bootstrap-vcpkg.sh
   ./vcpkg/vcpkg install
   ```

4. **Build the Project**:

   ```bash
   mkdir build
   cd build
   cmake .. -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake
   cmake --build .
   ```

### For Windows

1. **Clone the Repository**:

   ```cmd
   git clone https://github.com/lovyagin/compio.git
   cd C:\Users\Admin\compio
   ```

2. **Set Up vcpkg**:

    - Clone the vcpkg repository:

      ```cmd
      git clone https://github.com/microsoft/vcpkg.git
      cd vcpkg
      ```
    - Bootstrap vcpkg to generate the `vcpkg.exe` executable:

      ```cmd
      bootstrap-vcpkg.bat
      ```
    - Install dependencies:

      ```cmd
      vcpkg install
      ```
    - **Optional**: Add the vcpkg directory (e.g., `C:\Users\Admin\compio\vcpkg`) to your system's `PATH` environment variable for easier access to `vcpkg.exe`.

3. **Build the Project**:

    - Create a build directory:

      ```cmd
      mkdir build
      cd build
      ```
    - Configure the project with CMake, specifying the vcpkg toolchain file:

      ```cmd
      cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/Users/Admin/compio/vcpkg/scripts/buildsystems/vcpkg.cmake
      ```
    - Build the project:

      ```cmd
      cmake --build .
      ```
    - **Using an IDE (e.g., CLion)**: If you are using an IDE like CLion, open the project and add the following to your CMake options:

      ```
      -DCMAKE_TOOLCHAIN_FILE=C:/Users/Admin/compio/vcpkg/scripts/buildsystems/vcpkg.cmake
      ```

      Then, use the IDE's build tools to compile the project.

## Notes

- Ensure the path `C:/Users/Admin/compio/vcpkg/scripts/buildsystems/vcpkg.cmake` is adjusted to match your actual directory structure on Windows.
- If you encounter issues with dependencies, verify that vcpkg has installed all required packages by running `vcpkg install` in the vcpkg directory.
- For additional support or to report issues, visit the Compio GitHub repository.

## License

This project is licensed under the GNU General Public License v3.0. See the LICENSE file for details.