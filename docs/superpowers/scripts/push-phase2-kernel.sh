#!/bin/bash
# Push Phase 2 kernel extraction to neoos-kernel repo

set -e

echo "=== Pushing Kernel History to neoos-kernel ==="

cd /path/to/NeoOS/repo  # You must run this from the monorepo

# The kernel-only-history branch contains:
# - 222 kernel development commits (tcp, network, etc.)
# - Supporting files (lib, boot, tools, userland, docs, shim)

# Clone neoos-kernel repo
cd /tmp
rm -rf neoos-kernel-push
git clone https://github.com/NeoOSOrganization/neoos-kernel neoos-kernel-push
cd neoos-kernel-push

# Add monorepo as remote and fetch the kernel-only-history branch
git remote add monorepo /path/to/NeoOS/repo
git fetch monorepo kernel-only-history

# Force main branch to point to kernel-only-history
git checkout -b main-from-history monorepo/kernel-only-history
git push origin main-from-history:main -f

echo "✅ Kernel history pushed to neoos-kernel"
echo ""
echo "To verify:"
echo "  cd /tmp/neoos-kernel-push"
echo "  git log --oneline | head -5  # Should show tcp commits"
echo "  make MUSL_DIR=../neoos-musl/build-output  # Should build"
