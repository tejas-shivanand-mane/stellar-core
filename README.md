## Stellar-O3RUBC:

### Requirements

Same library dependencies as stellar-core

### Key Modified Files

OverlayManagerImpl.cpp

Stellar-overlay.x

Peer.cpp

Config.cpp

and their corresponding .h files

### Setup

The script base.sh contains helper commands to create the required node directories and configuration files.

These commands are initially commented out:

Uncomment them during the first setup.

After the setup is complete, re-comment them to avoid re-creating the directories on subsequent runs.