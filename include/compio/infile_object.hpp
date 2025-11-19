/**
 * @file infile_object.hpp
 * @brief Smart file-backed object system with reference counting and lazy loading
 *
 * This header provides a comprehensive system for managing objects that are stored
 * in files but can be manipulated in memory with automatic persistence. The system
 * includes:
 * - Endian-aware file I/O functions for cross-platform compatibility
 * - Abstract base class for file-serializable objects
 * - Smart pointer wrapper with reference counting and copy-on-write semantics
 * - Lazy loading and automatic write-back when objects are modified
 * - Support for benchmarking file operations
 */

#ifndef INFILE_OBJECT_HPP
#define INFILE_OBJECT_HPP

#include <cstdint>
#include <cstdio>
#include <type_traits>

/**
 * @brief Macro to create a const reference to a smart_infile_object
 *
 * This macro provides a convenient way to get read-only access to a smart_infile_object
 * by casting it to a const reference, preventing modification through the returned reference.
 *
 * @param x The smart_infile_object to make readonly
 * @param t The type parameter of the smart_infile_object
 */
#define readonly(x, t) (const_cast<const smart_infile_object<t> &>(x))

/**
 * @brief Write data to file in little-endian format
 *
 * Writes the specified number of elements to a file stream, ensuring little-endian
 * byte order regardless of the system's native endianness. This function handles
 * byte swapping for multi-byte data types (16-bit, 32-bit, 64-bit) when running
 * on big-endian systems.
 *
 * @param ptr Pointer to the data to write
 * @param size Size of each element in bytes
 * @param nmemb Number of elements to write
 * @param stream File stream to write to
 * @return Number of elements successfully written
 */
uint64_t lendian_fwrite(const void *ptr, uint64_t size, uint64_t nmemb, FILE *stream);

/**
 * @brief Read data from file in little-endian format
 *
 * Reads the specified number of elements from a file stream, ensuring little-endian
 * byte order regardless of the system's native endianness. This function handles
 * byte swapping for multi-byte data types (16-bit, 32-bit, 64-bit) when running
 * on big-endian systems.
 *
 * @param ptr Pointer to buffer where data will be stored
 * @param size Size of each element in bytes
 * @param nmemb Number of elements to read
 * @param stream File stream to read from
 * @return Number of elements successfully read
 */
uint64_t lendian_fread(void *ptr, uint64_t size, uint64_t nmemb, FILE *stream);

/**
 * @brief Abstract base class for file-serializable objects
 *
 * This class defines the interface for objects that can be serialized to and
 * deserialized from files. Derived classes must implement the pure virtual
 * methods to define how their data is read from and written to file storage.
 */
class infile_object {
public:
    /**
     * @brief Read object data from file at specified address
     *
     * Pure virtual method that must be implemented by derived classes to
     * read their state from a file at the given address.
     *
     * @param file File stream to read from
     * @param addr File address (offset) where the object data is stored
     */
    virtual void read_from(FILE *file, uint64_t addr) = 0;

    /**
     * @brief Write object data to file at specified address
     *
     * Pure virtual method that must be implemented by derived classes to
     * write their state to a file at the given address.
     *
     * @param file File stream to write to
     * @param addr File address (offset) where the object data should be stored
     */
    virtual void write_to(FILE *file, uint64_t addr) const = 0;

    /**
     * @brief Virtual destructor
     *
     * Ensures proper cleanup of derived classes through base class pointer.
     */
    virtual ~infile_object() = default;
};

/**
 * @brief Smart pointer wrapper for file-backed objects with reference counting
 *
 * This template class provides smart pointer semantics for objects that are stored
 * in files but can be manipulated in memory. It implements reference counting,
 * copy-on-write semantics, and lazy loading for efficient memory usage and
 * automatic persistence.
 *
 * Key features:
 * - Reference counting for shared ownership
 * - Copy-on-write semantics to avoid unnecessary file I/O
 * - Lazy loading: objects are read from file only when accessed
 * - Automatic write-back when modified objects are destroyed
 * - Pointer-like interface with operator-> and operator*
 * - Support for manually marking objects as removed or modified
 *
 * @tparam T Type of the managed object, must derive from infile_object
 */
template <typename T> class smart_infile_object {
    static_assert(std::is_base_of<infile_object, T>::value, "T must be derived from infile_object");

private:
    /**
     * @brief Internal storage structure with reference counting
     *
     * This structure manages the actual object data, file information,
     * and state flags. It implements reference counting to allow multiple
     * smart_infile_object instances to share the same underlying data.
     */
    struct storage {
        int ref_count; /**< Reference count for shared ownership */
        bool modified; /**< Flag indicating if data has been modified */
        bool removed;  /**< Flag indicating if object should be removed */
        T *data;       /**< Pointer to the actual object data */
        FILE *file;    /**< File stream */
        uint64_t addr; /**< File address where object is stored */

        /**
         * @brief Construct storage with existing data
         *
         * Creates a storage structure with pre-existing object data.
         * The object is initially marked as modified since it was
         * created with external data.
         *
         * @param file File stream
         * @param addr File address where object is stored
         * @param data Pointer to existing object data
         */
        storage(FILE *file, uint64_t addr, T *data)
            : ref_count(1),
              modified(true),
              removed(false),
              data(data),
              file(file),
              addr(addr) {}

        /**
         * @brief Construct storage by reading from file
         *
         * Creates a storage structure and reads the object data from file.
         * The object is initially marked as not modified since it was
         * just read from storage.
         *
         * @param file File stream to read from
         * @param addr File address where object data is stored
         */
        storage(FILE *file, uint64_t addr) : storage(file, addr, new T()) {
            modified = false;
            read();
        }

        /**
         * @brief Destructor with automatic write-back
         *
         * If the object has been modified and not marked for removal,
         * writes the data back to file before cleaning up.
         */
        ~storage() {
            if (modified && !removed) {
                write();
            }
            delete data;
        }

        /**
         * @brief Read object data from file
         *
         * Calls the object's read_from method to load data from file.
         */
        void read() { data->read_from(file, addr); }

        /**
         * @brief Write object data to file
         *
         * Calls the object's write_to method to save data to file.
         */
        void write() { data->write_to(file, addr); }
    };

    storage *S; /**< Pointer to the shared storage structure */

public:
    /**
     * @brief Default constructor
     *
     * Creates an empty smart_infile_object that doesn't point to any data.
     */
    smart_infile_object() : S(nullptr) {}

    /**
     * @brief Construct with existing object data
     *
     * Creates a smart_infile_object that takes ownership of existing object data
     * and associates it with a file location.
     *
     * @param file File stream
     * @param addr File address where object is stored
     * @param data Pointer to existing object data (ownership is transferred)
     */
    smart_infile_object(FILE *file, uint64_t addr, T *data) : S(new storage(file, addr, data)) {}

    /**
     * @brief Construct by reading from file
     *
     * Creates a smart_infile_object and reads the object data from the specified
     * file location. The object is created using its default constructor and then
     * populated with data from file.
     *
     * @param file File stream to read from
     * @param addr File address where object data is stored
     */
    smart_infile_object(FILE *file, uint64_t addr) : S(new storage(file, addr)) {}

    /**
     * @brief Copy constructor
     *
     * Creates a new smart_infile_object that shares the same underlying data
     * as the source object. The reference count is incremented.
     *
     * @param other The smart_infile_object to copy from
     */
    smart_infile_object(const smart_infile_object &other) { *this = other; }

    /**
     * @brief Move constructor
     *
     * Creates a new smart_infile_object by moving the data from the source object.
     * The source object is left in a valid but unspecified state.
     *
     * @param other The smart_infile_object to move from
     */
    smart_infile_object(smart_infile_object &&other) { *this = other; }

    /**
     * @brief Copy assignment operator
     *
     * Shares the underlying data with the source object and increments the
     * reference count. If this object previously pointed to data, that data's
     * reference count is decremented and cleaned up if it reaches zero.
     *
     * @param other The smart_infile_object to copy from
     * @return Reference to this object
     */
    smart_infile_object &operator=(const smart_infile_object &other) {
        S = other.S;
        ++S->ref_count;
        return *this;
    }

    /**
     * @brief Move assignment operator
     *
     * Transfers ownership of the data from the source object to this object.
     * The source object is left in a valid but unspecified state.
     *
     * @param other The smart_infile_object to move from
     * @return Reference to this object
     */
    smart_infile_object &operator=(smart_infile_object &&other) {
        std::swap(S, other.S);
        return *this;
    }

    /**
     * @brief Destructor
     *
     * Decrements the reference count of the shared storage. If the reference
     * count reaches zero, the storage (and the contained object) is destroyed.
     */
    ~smart_infile_object() {
        if (S != nullptr) {
            --S->ref_count;
            if (S->ref_count == 0)
                delete S;
        }
    }

    /**
     * @brief Get the file address of the object
     *
     * @return File address where the object is stored
     */
    uint64_t addr() const { return S->addr; }

    /**
     * @brief Get raw pointer to the object data
     *
     * Returns a pointer to the underlying object data and marks the object
     * as modified. This allows direct manipulation of the object.
     *
     * @return Pointer to the object data, or nullptr if no object is loaded
     */
    T *ptr() const {
        if (S == nullptr) {
            return nullptr;
        }
        S->modified = true;
        return S->data;
    }

    /**
     * @brief Const arrow operator
     *
     * Provides read-only access to the object's members through pointer syntax.
     *
     * @return Const pointer to the object data
     */
    const T *operator->() const { return S->data; }

    /**
     * @brief Arrow operator
     *
     * Provides read-write access to the object's members through pointer syntax.
     * Marks the object as modified when called.
     *
     * @return Pointer to the object data
     */
    T *operator->() {
        S->modified = true;
        return S->data;
    }

    /**
     * @brief Const dereference operator
     *
     * Provides read-only access to the object through reference syntax.
     *
     * @return Const reference to the object data
     */
    const T &operator*() const { return *S->data; }

    /**
     * @brief Dereference operator
     *
     * Provides read-write access to the object through reference syntax.
     * Marks the object as modified when called.
     *
     * @return Reference to the object data
     */
    T &operator*() {
        S->modified = true;
        return *S->data;
    }

    /**
     * @brief Boolean conversion operator
     *
     * Checks if the smart_infile_object points to valid data.
     *
     * @return true if the object contains valid data, false otherwise
     */
    operator bool() const { return S != nullptr; }

    /**
     * @brief Mark the object for removal
     *
     * Sets the removed flag, indicating that the object should be removed
     * from storage when the reference count reaches zero.
     */
    void remove() { S->removed = true; }

    /**
     * @brief Mark the object as modified
     *
     * Explicitly marks the object as modified, ensuring it will be
     * written back to file when destroyed.
     */
    void modify() { S->modified = true; }

    /**
     * @brief Mark the object as not modified (development only)
     *
     * Clears the modified flag. This is intended for development purposes
     * and should be used with caution as it may prevent changes from being
     * persisted to file. It is used in btree.cpp to ensure, that we don't write index_nodes to
     * file, when archive is open in read-only mode, but we still need to modify index_nodes to
     * apply parent key_additions.
     */
    void unmodify() const { S->modified = false; }

    /**
     * @brief Read object data from file
     *
     * Reloads the object data from file, discarding any unsaved modifications.
     */
    void read() { S->read(); }

    /**
     * @brief Write object data to file
     *
     * Immediately writes the current object state to file, regardless of
     * the modified flag.
     */
    void write() { S->write(); }
};

#ifdef COMPIO_BENCHMARK_FILE_OPERATIONS_COUNTER
/**
 * @brief Get the total number of bytes read
 *
 * Returns the cumulative count of bytes read through lendian_fread
 * since the program started or since the counter was last reset.
 *
 * @return Total number of bytes read
 */
int get_n_read_bytes();

/**
 * @brief Get the total number of bytes written
 *
 * Returns the cumulative count of bytes written through lendian_fwrite
 * since the program started or since the counter was last reset.
 *
 * @return Total number of bytes written
 */
int get_n_written_bytes();
#endif

#endif // INFILE_OBJECT_HPP
