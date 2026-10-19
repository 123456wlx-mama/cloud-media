#include "logic/block_sender_factory.h"

namespace cm {
namespace logic {

BlockSenderKind BlockSenderFactory::select() {
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    return BlockSenderKind::Sendfile;
#else
    return BlockSenderKind::Buffered;
#endif
}

}  // namespace logic
}  // namespace cm
