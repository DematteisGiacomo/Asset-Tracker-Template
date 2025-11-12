# Asset Tracker Template Webinar

## Building Production-Ready Cellular IoT Applications with Nordic Semiconductor

---

## Description

**Transform your IoT ideas into production-ready solutions in record time!**

Join us for an in-depth technical webinar where you'll discover how to build robust, battery-powered cellular IoT applications using the **Asset Tracker Template** - Nordic Semiconductor's open-source framework for nRF91 Series devices.

Whether you're tracking assets across continents, monitoring environmental conditions remotely, or building your next innovative IoT product, this webinar will equip you with the knowledge and tools to accelerate your development journey from prototype to production.

### What You'll Learn

In this hands-on technical session, you'll discover:

- **Rapid Development**: How to go from idea to working prototype in hours, not weeks, using a proven modular architecture
- **Production-Ready Code**: Best practices for building maintainable, testable, and scalable IoT applications
- **Power Optimization**: Proven techniques to maximize battery life in cellular IoT devices (achieving months to years on a single charge)
- **Cloud Connectivity**: Seamless integration with nRF Cloud and strategies for connecting to custom cloud platforms
- **Location Services**: Implementing GNSS, Wi-Fi, and cellular positioning for comprehensive asset tracking
- **Real-World Deployment**: Over-the-air firmware updates (FOTA), remote debugging, and production monitoring

### Who Should Attend

This webinar is ideal for:

- **Embedded Developers** looking to accelerate cellular IoT development
- **Product Managers** evaluating IoT platforms and technologies
- **IoT Architects** designing scalable, maintainable systems
- **Hardware Engineers** working with nRF91 Series devices (nRF9151, nRF9160, nRF9161)
- **Anyone** interested in learning modern approaches to IoT application development

### Prerequisites

- Basic understanding of embedded C programming (recommended)
- Familiarity with IoT concepts helpful but not required
- No prior experience with Nordic devices necessary - we'll cover the essentials!

### What You'll Take Away

- **Open-source template** ready to customize for your use case
- **Reference architecture** for modular, event-driven IoT systems
- **Best practices guide** for power optimization and cloud connectivity
- **Access to recordings** and supplementary materials
- **Community access** for ongoing support and collaboration

---

## Webinar Agenda


### Part 1: Introduction & Overview

**1.1 Welcome & Context (5 min)**
- The challenge of building production-ready IoT applications
- Why cellular IoT? LTE-M and NB-IoT overview
- Introduction to nRF91 Series and supported hardware

**1.2 Asset Tracker Template Overview (10 min)**
- What is the Asset Tracker Template?
- Key features and benefits
- Real-world use cases and success stories
- Live demo: See it in action!

### Part 2: Architecture Deep Dive

**2.1 Modular Architecture Principles (10 min)**
- Event-driven design using Zephyr's zbus messaging
- State Machine Framework (SMF) for predictable behavior
- Thread management and blocking vs. non-blocking operations
- Module independence and loose coupling

**2.2 Core Modules Tour (10 min)**
- **Main Module**: Your application's business logic
- **Network Module**: LTE connectivity management
- **Cloud Module**: nRF Cloud CoAP communication
- **Location Module**: Multi-source positioning
- **Storage Module**: Data buffering and forwarding
- **Environmental, Button, LED, Power, FOTA modules**

### Part 3: Power Optimization

**3.1 Battery Life Fundamentals (7 min)**
- Understanding cellular IoT power consumption
- Power Saving Mode (PSM) and eDRX explained
- The impact of network operations on battery life

**3.2 Optimization Strategies (8 min)**
- Default power-saving configurations in the template
- Update interval tuning for your use case
- Buffer mode vs. passthrough mode trade-offs
- Peripheral power management (UART, sensors, etc.)
- Real measurements: Power Profiler Kit 2 (PPK2) demonstration

### Part 4: Cloud Connectivity & Location Services

**4.1 nRF Cloud Integration (5 min)**
- Device provisioning and attestation
- CoAP protocol overview
- Data serialization with CBOR
- Cloud-based device configuration

**4.2 Location Services (5 min)**
- GNSS positioning fundamentals
- Wi-Fi and cellular-based location
- Location method priorities and fallback strategies
- Cloud-assisted location services

### Part 5: Development Workflow & Best Practices

**5.1 Getting Started (8 min)**
- Development environment setup (nRF Connect SDK)
- Building and flashing for Thingy:91 X and nRF9151 DK
- Using VS Code extension for streamlined development
- Quick Start app for guided provisioning

**5.2 Customization & Extension (7 min)**
- Modifying business logic in the Main module
- Adding custom sensors and modules
- Kconfig configuration system
- Enabling MQTT support for custom clouds
- Testing and CI/CD with GitHub Actions

### Part 6: Production Readiness

**6.1 Firmware Updates (FOTA) (5 min)**
- Over-the-air update architecture
- Application and modem firmware updates
- Update scheduling and rollback strategies

**6.2 Debugging & Monitoring (5 min)**
- Shell commands for runtime diagnostics
- Memfault integration for remote debugging
- Modem trace collection and analysis
- Common issues and troubleshooting techniques

### Part 7: Next Steps & Resources

- Getting started with the template today
- Nordic Developer Academy courses
- Documentation and community support
- GitHub repository and contribution guidelines
- Upcoming features and roadmap

### Q&A Session

Interactive question and answer session with Nordic engineers



### Draft Content

#### Modular Architecture (Part 2)

**Why it matters:**
"Traditional embedded firmware often becomes a tangled mess of dependencies as it grows. We've solved this with a modular, event-driven architecture where each module has a single responsibility and communicates through well-defined message channels."

**Zbus messaging:**
"Think of zbus as an internal pub-sub system for your firmware. Modules publish messages on channels, and other modules subscribe to those channels. This means you can add, remove, or modify modules without breaking the entire system."

**State Machine Framework:**
"Each module uses Zephyr's State Machine Framework, which gives you predictable, testable behavior. The run-to-completion model means your state transitions happen atomically - no race conditions, no half-finished state changes. This is critical for production reliability."

**Demo scenario:**
"Let me show you how this works in practice. When you press a button on the Thingy:91 X, the Button module publishes a message. The Main module receives it, triggers a location search through the Location module, collects sensor data from the Environmental module, controls LED feedback, and finally sends everything to the cloud through the Cloud module. Each module is independent, testable, and maintainable."

---

#### Power Optimization (Part 3)

**The challenge:**
"Battery life is often THE defining factor for cellular IoT success. A device that needs battery replacement every few weeks is not commercially viable. The good news? With proper optimization, you can achieve months or even years of battery life."

**Power Saving Mode (PSM):**
"PSM is your best friend. When enabled, the modem enters a deep sleep state between transmissions. The template comes with PSM enabled by default with sensible parameters: 30-minute periodic TAU and 60-second active timer. During PSM, your device can consume microamps instead of milliamps."

**Configuration strategy:**
"The key is alignment: set your periodic TAU longer than your application's update interval. If you're transmitting data every hour, don't wake the modem every 30 minutes for tracking updates. This simple principle can double your battery life."

**Buffer mode:**
"Here's a powerful feature: buffer mode. Instead of connecting to the network every time you sample data, store it locally and transmit in batches. Sample every 10 minutes, transmit every hour - that's 6x fewer network connections, and network operations are where you spend most power."

**Real numbers:**
"With default settings on a Thingy:91 X, transmitting location and sensor data every hour, you can expect 6+ months on the built-in battery. Increase to 4-hour intervals with buffer mode, and you're looking at over a year. These are real, measured values available in our documentation."

---

#### Cloud Connectivity (Part 4)

**nRF Cloud integration:**
"The template uses nRF Cloud with CoAP - a lightweight protocol perfect for constrained devices. But here's what's really powerful: device attestation. Your devices come with cryptographic identities from the factory. No manual certificate management, no security keys to lose. Just claim your device with an attestation token, and it's provisioned automatically."

**Data serialization:**
"We use CBOR - Concise Binary Object Representation - for all cloud communication. Why? It's significantly smaller than JSON, which means less data over the air, lower costs, and faster transmissions. Plus, we've included CDDL schemas so you can validate and evolve your data format as your product grows."

**Configurability:**
"One of my favorite features: you can reconfigure devices from the cloud. Want to change the update interval for a deployed fleet? Send a device shadow update from nRF Cloud. No firmware update required. This has saved our customers countless field visits."

**Custom clouds:**
"While we default to nRF Cloud, the template includes an MQTT example module. You can connect to AWS IoT, Azure IoT Hub, or your own MQTT broker. The modular architecture means you can swap the cloud backend without touching your core application logic."

---

#### Location Services (Part 4)

**Multi-source positioning:**
"Asset tracking is more than just GPS. The template supports three location methods:
- GNSS for high accuracy outdoors
- Wi-Fi positioning for indoor and urban environments  
- Cellular positioning when GNSS isn't available

You configure priorities, and the system automatically falls back if the primary method fails."

**Power-efficient location:**
"GNSS is accurate but power-hungry. The template includes intelligent timeout management and cloud-assisted GNSS (A-GNSS) to reduce time-to-first-fix. We've seen fix times drop from 60 seconds to under 10 seconds with A-GNSS, which directly translates to better battery life."

**Cloud-assisted location:**
"For Wi-Fi and cellular positioning, the device scans for nearby signals and sends them to the cloud for location calculation. This offloads the heavy computation from your device and gives you location estimates in seconds with minimal power consumption."

---

#### Development Workflow (Part 5)

**Getting started:**
"We've worked hard to make getting started as smooth as possible. If you're using VS Code with the nRF Connect extension - which I highly recommend - you can build and flash the template in just a few clicks. We also have a Quick Start app in nRF Connect for Desktop that walks you through the entire setup and provisioning process."

**Customization:**
"The beauty of this template is that it's designed to be customized. The main business logic lives in the Main module - that's where you'll spend most of your time. Want to add a custom sensor? There's a guide for that. Need to implement custom behavior? Modify the state machine. Want to add a completely new module? Follow the established patterns, and it'll integrate seamlessly."

**Kconfig system:**
"Everything is configurable through Kconfig - the same system the Linux kernel uses. Want to change update intervals? It's a config option. Need to disable a module you don't need? One line in your config file. This makes it easy to maintain variants for different products or customers."

**Testing:**
"Production readiness means testing. The template includes comprehensive unit tests, and we've set up GitHub Actions for continuous integration. Every module has its own test suite. You can run these tests locally or integrate them into your own CI/CD pipeline."

---

#### Production Readiness (Part 6)

**FOTA (Firmware Over-The-Air):**
"Your devices are deployed. How do you update them? FOTA support is built into the template. You can update application firmware, modem firmware, or both. Updates are retrieved from nRF Cloud and applied with built-in validation and rollback. Schedule updates during maintenance windows, and monitor progress through the cloud dashboard."

**Remote debugging:**
"When something goes wrong in the field, you need visibility. The template integrates with Memfault for remote debugging. Crash dumps, performance metrics, event traces - all automatically collected and uploaded. You can diagnose issues without physically touching the device."

**Modem tracing:**
"For connectivity issues, modem traces are invaluable. The template can capture modem traces and upload them to Memfault for analysis. These traces show exactly what's happening at the protocol level - network negotiations, signal quality, errors. It's like having a protocol analyzer attached to every deployed device."

**Shell commands:**
"During development and debugging, the integrated shell gives you runtime access to the device. Query network status, trigger operations manually, adjust log levels, read sensor values - all without rebuilding firmware. It's an incredibly powerful tool for troubleshooting."

---

#### Real-World Use Cases

**Asset tracking:**
"Obviously! Track shipping containers, rental equipment, vehicles, valuable cargo - anything that moves. The multi-source location capability means you get positioning even when devices move from outdoor to indoor environments."

**Environmental monitoring:**
"Deploy sensors in remote locations for agriculture, weather monitoring, environmental compliance. The power optimization features mean you can place devices where power infrastructure doesn't exist, and the cellular connectivity means you get data wherever there's cellular coverage."

**Fleet management:**
"Monitor vehicle fleets with location tracking, sensor data collection, and cloud connectivity. The template's modularity makes it easy to add custom sensors like fuel level, door status, or cargo temperature."

**Smart agriculture:**
"Monitor soil conditions, weather, equipment status across large farms. The buffer mode is perfect here - sample frequently for accurate data, transmit less frequently for better battery life."

**Cold chain monitoring:**
"Track temperature-sensitive shipments with environmental sensors, location tracking, and cloud alerts. The reliable cloud connectivity ensures you're notified immediately if conditions go out of range."
