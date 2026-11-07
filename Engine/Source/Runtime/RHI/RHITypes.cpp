module;

#include <magic_enum/magic_enum_flags.hpp>

module RHI.Types;

import Core;
import RHI.Ref;
import magic_enum;

import std;

namespace SoulEngine {

auto RHITextureView::Create(RHIRef<RHITexture>  Texture,
                            RHITextureUsage     Usage,
                            RHIAccess           Access,
                            RHISubresourceRange Range) -> std::expected<RHITextureView, ErrorMessage> {
    const auto* Resource = Texture.TryGet();
    if (!Resource)
        return std::unexpected(ErrorMessage("Texture view requires a ready texture"));
    if (!std::has_single_bit(static_cast<Uint32>(Usage)))
        return std::unexpected(ErrorMessage("Texture view requires exactly one usage role"));
    if (!magic_enum::enum_flags_test(Resource->GetUsage(), Usage))
        return std::unexpected(ErrorMessage("Texture view usage exceeds its creation usage"));
    const auto AccessBits      = static_cast<Uint8>(Access);
    const auto ValidAccessBits = static_cast<Uint8>(RHIAccess::Read) | static_cast<Uint8>(RHIAccess::Write);
    if (AccessBits == 0 || (AccessBits & ~ValidAccessBits) != 0)
        return std::unexpected(ErrorMessage("Texture view access is invalid"));
    if (Range.BaseMipLevel >= Resource->GetMipLevelCount() || Range.MipLevelCount == 0 ||
        (Range.MipLevelCount != kRHIRemainingSubresources &&
         Range.MipLevelCount > Resource->GetMipLevelCount() - Range.BaseMipLevel))
        return std::unexpected(ErrorMessage("Texture view mip range exceeds its texture"));
    return RHITextureView(std::move(Texture), Usage, Access, Range);
}

auto RHIBufferView::Create(RHIRef<RHIBuffer> Buffer,
                           RHIBufferUsage    Usage,
                           RHIAccess         Access,
                           Uint64            Offset,
                           Uint64            Range) -> std::expected<RHIBufferView, ErrorMessage> {
    const auto* Resource = Buffer.TryGet();
    if (!Resource)
        return std::unexpected(ErrorMessage("Buffer view requires a ready buffer"));
    if (!std::has_single_bit(static_cast<Uint32>(Usage)))
        return std::unexpected(ErrorMessage("Buffer view requires exactly one usage role"));
    if (!magic_enum::enum_flags_test(Resource->GetUsage(), Usage))
        return std::unexpected(ErrorMessage("Buffer view usage exceeds its creation usage"));
    const auto AccessBits      = static_cast<Uint8>(Access);
    const auto ValidAccessBits = static_cast<Uint8>(RHIAccess::Read) | static_cast<Uint8>(RHIAccess::Write);
    if (AccessBits == 0 || (AccessBits & ~ValidAccessBits) != 0)
        return std::unexpected(ErrorMessage("Buffer view access is invalid"));
    if (Offset > Resource->GetSize())
        return std::unexpected(ErrorMessage("Buffer view offset exceeds its buffer"));
    const Uint64 ViewRange = Range == kRHIWholeBuffer ? Resource->GetSize() - Offset : Range;
    if (ViewRange == 0 || ViewRange > Resource->GetSize() - Offset)
        return std::unexpected(ErrorMessage("Buffer view range exceeds its buffer"));
    return RHIBufferView(std::move(Buffer), Usage, Access, Offset, Range);
}

} // namespace SoulEngine
