#include <cstdint>
#include <vector>

namespace VulkanEngine
{
    struct MeshHandle
    {
        uint32_t id;
    };

    class MeshHandler
    {
    public:
        MeshHandle LoadMesh(const char* filePath);
        void UnloadMesh(MeshHandle handle);

        const std::vector<MeshHandle> GetAllMeshes() const;
    private:
    
    };
} // namespace VulkanEngine
