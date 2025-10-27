export function Name() { return "Corsair Headset Device"; }
export function VendorId() { return 0x1B1C; }
export function ProductId() { return Object.keys(CORSAIRdeviceLibrary.PIDLibrary); }
export function Publisher() { return "WhirlwindFx"; }
export function Documentation(){ return "troubleshooting/corsair"; }
export function Size() { return [1, 1]; }
export function DeviceType(){return "headphones";}
export function Validate(endpoint) { return endpoint.interface === 3 || endpoint.interface === 4; }
export function ImageUrl() { return "https://assets.signalrgb.com/devices/default/misc/usb-drive-render.png"; }
/* global
LightingMode:readonly
forcedColor:readonly
micLedMode:readonly
micMuteColor:readonly
idleTimeout:readonly
*/
export function ControllableParameters() {
	return [
		{property:"LightingMode", group:"lighting", label:"Lighting Mode", description: "Determines where the device's RGB comes from. Canvas will pull from the active Effect, while Forced will override it to a specific color", type:"combobox", values:["Canvas", "Forced"], default:"Canvas"},
		{property:"forcedColor", group:"lighting", label:"Forced Color", description: "The color used when 'Forced' Lighting Mode is enabled", min:"0", max:"360", type:"color", default:"#009bde"},
		{property:"micLedMode", group:"lighting", label:"Microphone LED Mode", description: "Sets the microphone LED behavior", type:"combobox", values:["Canvas", "MuteState"], default:"Canvas"},
		{property:"micMuteColor", group:"lighting", label:"Microphone Mute Color", description: "Sets the microphone LED color when on mute while 'Microphone LED Mode' is set to 'MuteState'", min:"0", max:"360", type:"color", default:"#ff0000"},
		//{property:"SidetoneAmount", group:"", label:"Sidetone", description: "Sets the sidetone level amount", step:"1", type:"number", min:"0", max:"100", default:"0", live : false}, // Looks like not all models works with this, disabling for now, looks like to not be used that much
	];
}

export function Initialize() {
	CORSAIR.Initialize();
}

export function Render() {
	if (CORSAIR.getWirelessSupport()){

		CORSAIR.fetchStatus();

		if (!CORSAIR.Config.isSleeping){
			CORSAIR.sendColors();
		}

		CORSAIR.fetchBattery();
	} else {
		CORSAIR.sendColors();
	}
}

export function Shutdown(SystemSuspending) {

	if(SystemSuspending){
		// Go Dark on System Sleep/Shutdown
		CORSAIR.sendColors("#000000");
	}else{
		const headsetMode = CORSAIR.getWirelessSupport() === true ? 0x09 : 0x08;
		device.write([0x02, headsetMode, 0x01, 0x03, 0x00, 0x01], 64); // Hardware mode
	}
}

/*
export function onSidetoneAmountChanged() {
	CORSAIR.setSidetone();
}
*/

export function onidleTimeoutChanged() {
	CORSAIR.setIdleTimeout();
}

export class CORSAIR_Device_Protocol {
	constructor() {
		this.Config = {
			DeviceProductID: 0x0000,
			DeviceName: "Corsair Headset Device",
			DeviceEndpoint: { "interface": 0, "usage": 0x0000, "usage_page": 0x0000, "collection": 0x0000 },
			LedNames: [],
			LedPositions: [],
			Leds: [],
			Wireless: false, // True equals to 0x09 and false to 0x08,
			pollingInterval: 1000,
			lastMicStatePolling: 0,
			lastMicState: 0,
			lastBatteryPolling: 0,
			pollingBatteryInterval: 60000, // 1 Minute
			lastpollingHeadsetStatus: 0,
			pollingHeadsetStatus: 10000, // 10 seconds
		};

		this.chargingStates = Object.freeze({
			1: "Charging",
			2: "Discharging",
			3: "Fully Charged",
		});

		this.chargingStateDictionary = Object.freeze({
			1 : 2,
			2 : 1,
			3 : 4
		});
	}

	getDeviceProperties(deviceID) { return CORSAIRdeviceLibrary.PIDLibrary[deviceID];};

	getDeviceProductId() { return this.Config.DeviceProductID; }
	setDeviceProductId(productID) { this.Config.DeviceProductID = productID; }

	getDeviceName() { return this.Config.DeviceName; }
	setDeviceName(deviceName) { this.Config.DeviceName = deviceName; }

	getDeviceEndpoint() { return this.Config.DeviceEndpoint; }
	setDeviceEndpoint(deviceEndpoint) { this.Config.DeviceEndpoint = deviceEndpoint; }

	getLedNames() { return this.Config.LedNames; }
	setLedNames(ledNames) { this.Config.LedNames = ledNames; }

	getLedPositions() { return this.Config.LedPositions; }
	setLedPositions(ledPositions) { this.Config.LedPositions = ledPositions; }

	getLeds() { return this.Config.Leds; }
	setLeds(leds) { this.Config.Leds = leds; }

	getDeviceImage() { return this.Config.image; }
	setDeviceImage(image) { this.Config.image = image; }

	getWirelessSupport() { return this.Config.Wireless; }
	setWirelessSupport(wireless) { this.Config.Wireless = wireless; }

	Initialize() {
		//Initializing vars
		this.setDeviceProductId(device.productId());

		const DeviceProperties = this.getDeviceProperties(this.getDeviceProductId());
		this.setDeviceName(DeviceProperties.name);
		this.detectDeviceEndpoint(DeviceProperties);
		this.setLedNames(DeviceProperties.LedNames);
		this.setLedPositions(DeviceProperties.LedPositions);
		this.setLeds(DeviceProperties.Leds);
		this.setDeviceImage(DeviceProperties.image);

		if(DeviceProperties.wireless){
			this.setWirelessSupport(DeviceProperties.wireless);
			device.addFeature("battery");
			device.addProperty({"property":"idleTimeout", "group":"", "label":"Device Sleep Timeout (Minutes)", description: "Enables the device to enter sleep mode", "type":"combobox", "values":["Off", "1", "2", "3", "4", "5", "10", "15", "20", "25", "30"], "default":"10"});
			CORSAIR.fetchBattery(true);
		}

		device.log("Device model found: " + this.getDeviceName());
		device.setName("Corsair " + this.getDeviceName());
		device.setSize(DeviceProperties.size);
		device.setControllableLeds(this.getLedNames(), this.getLedPositions());
		device.setImageFromUrl(this.getDeviceImage());

		this.modernDirectLightingMode();
	}

	modernDirectLightingMode() {
		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint[`interface`], endpoint[`usage`], endpoint[`usage_page`], endpoint[`collection`]);

		console.log("Setting Software Mode!");
		device.write([0x02, headsetMode, 0x01, 0x03, 0x00, 0x02], 64); // Enable Software Mode
		device.pause(100);
		device.write([0x02, headsetMode, 0x0D, 0x00, 0x01], 64); //Open lighting endpoint
		device.pause(100);

		const HWBrightnessPacket = [0x02, headsetMode, 0x02, 0x02, 0x00];

		device.clearReadBuffer();
		device.write(HWBrightnessPacket, 64);
		device.pause(100);

		const HWBrightness = device.read(HWBrightnessPacket, 64);

		if (HWBrightness[4] !== 0xe8 || HWBrightness[5] !== 0x03) {
			device.write([0x02, headsetMode, 0x01, 0x02, 0x00, 0xe8, 0x03], 64); //Hardware Brightness 100%
		}
	}

	sendColors(overrideColor) {

		const deviceLedPositions	= this.getLedPositions();
		const deviceLedNames		= this.getLedNames();
		const deviceLeds			= this.getLeds();
		const RGBData				= [];

		for (let iIdx = 0; iIdx < deviceLeds.length; iIdx++) {
			const iPxX = deviceLedPositions[iIdx][0];
			const iPxY = deviceLedPositions[iIdx][1];
			let color;

			if(deviceLedNames[iIdx] === "Mic" && micLedMode === "MuteState"){
				const micState = this.fetchMicStatus();

				if (micState === 1) {
					color = hexToRgb(micMuteColor);
				} else {
					color = device.color(iPxX, iPxY);
				}
			}else if (overrideColor){
				color = hexToRgb(overrideColor);
			}else if (LightingMode === "Forced") {
				color = hexToRgb(forcedColor);
			}else {
				color = device.color(iPxX, iPxY);
			}

			RGBData[(deviceLeds[iIdx])]   = color[0];
			RGBData[(deviceLeds[iIdx])+3] = color[1];
			RGBData[(deviceLeds[iIdx])+6] = color[2];
		}

		this.writeRGB(RGBData);
	}

	writeRGB(RGBData) {
		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint[`interface`], endpoint[`usage`], endpoint[`usage_page`], endpoint[`collection`]);

		device.write([0x02, headsetMode, 0x06, 0x00, 0x09, 0x00, 0x00, 0x00].concat(RGBData), 64);
	}

	fetchMicStatus(){

		if(Date.now() - this.Config.lastMicStatePolling < this.Config.pollingInterval) {
			return this.Config.lastMicState;
		}

		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const micreadMode = this.getDeviceName().includes("HS80") === true ? 0xA6 : 0x46;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint[`interface`], endpoint[`usage`], endpoint[`usage_page`], endpoint[`collection`]);

		const micStatusPacket = [0x02, headsetMode, 0x02, micreadMode, 0x00];

		device.pause(100);
		device.clearReadBuffer();
		device.write(micStatusPacket, 64);
		device.pause(50);

		// Index 4 returns 0 to unmutted/enable and 1 to muted/disabled
		// There are some cases where the index changes to 5, not sure if it's a bug or specific condition
		const micStatus = device.read(micStatusPacket, 64);
		this.Config.lastMicStatePolling = Date.now();

		if (micStatus[3] === micreadMode) {
			this.fetchSleepStatus();

			return this.Config.lastMicState;
		}

		this.Config.lastMicState = micStatus[4];

		return micStatus[4];

	}

	/*
	setSidetone() {
		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint[`interface`], endpoint[`usage`], endpoint[`usage_page`], endpoint[`collection`]);

		const sidetoneValue = Math.round((SidetoneAmount / 100) * 1000);

		console.log("Setting Sidetone to: " + SidetoneAmount);
		device.write([0x02, headsetMode, 0x01, 0x47, 0x00, sidetoneValue & 0xFF, (sidetoneValue >> 8) & 0xFF], 64);
	}
	*/

	setIdleTimeout() {
		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint[`interface`], endpoint[`usage`], endpoint[`usage_page`], endpoint[`collection`]);

		if (idleTimeout === "Off") {
			console.log ("Setting Idle Timeout to: disabled");
			device.write([0x02, headsetMode, 0x01, 0x0D], 64);
		} else {
			device.write([0x02, headsetMode, 0x01, 0x0D, 0x01], 64);
			device.pause(1);

			device.write([0x02, headsetMode, 0x01, 0x0D, 0x00, 0x01], 64);
			device.pause(1);

			const timeoutValue = idleTimeout * 60000;
			const hexValue = timeoutValue.toString(16).padStart(6, '0');
			const littleEndianHex = hexValue.match(/../g).reverse();

			const packet = [];
			packet[0] = 0x02;
			packet[1] = headsetMode;
			packet[2] = 0x01;
			packet[3] = 0x0e;

			packet[5] = parseInt(littleEndianHex[0], 16);
			packet[6] = parseInt(littleEndianHex[1], 16);
			packet[7] = parseInt(littleEndianHex[2], 16);

			console.log ("Setting Idle Timeout to: " + idleTimeout);
			device.write(packet, 64);
		}
	}

	fetchBattery(force = false){

		if(!force && Date.now() - this.Config.lastBatteryPolling < this.Config.pollingBatteryInterval) {
			return;
		}

		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint[`interface`], endpoint[`usage`], endpoint[`usage_page`], endpoint[`collection`]);

		const batteryLevelPacket = [0x02, headsetMode, 0x02, 0x0F, 0x00];
		const batteryStatusPacket = [0x02, headsetMode, 0x02, 0x10, 0x00];

		device.pause(1000);
		device.clearReadBuffer();
		device.write(batteryLevelPacket, 64);

		device.pause(50);
		device.clearReadBuffer();
		device.write(batteryStatusPacket, 64);

		const batteryLevelData	= device.read(batteryLevelPacket, 64);
		const batteryStatusData = device.read(batteryStatusPacket, 64);

		const batteryLevel	=	this.ReadInt32LittleEndian(batteryLevelData.slice(4, 7));
		const batteryStatus	=	batteryStatusData[4];

		this.Config.lastBatteryPolling	= Date.now();

		device.log(`Battery Level is [${(batteryLevel ?? 0)/10}%]`);
		device.log(`Battery Status is [${this.chargingStates[batteryStatus ?? 0]}]`);

		battery.setBatteryLevel((batteryLevel ?? 0)/ 10);
		battery.setBatteryState(this.chargingStateDictionary[batteryStatus ?? 0]);
	}

	fetchSleepStatus(){
		const headsetMode = this.getWirelessSupport() === true ? 0x09 : 0x08;
		const endpoint = this.getDeviceEndpoint();
		device.set_endpoint(endpoint.interface, endpoint.usage, endpoint.usage_page, endpoint.collection);

		const batteryStatusPacket = [0x02, headsetMode, 0x02, 0x10, 0x00];

		device.clearReadBuffer();
		device.write(batteryStatusPacket, 64);
		device.pause(100);

		const batteryStatusData = device.read(batteryStatusPacket, 64);

		const wasSleeping = this.Config.isSleeping;
		this.Config.isSleeping = batteryStatusData[0] === 2;

		if (this.Config.isSleeping) {
			device.log("Headset is currently sleeping.");
		} else {
			if (wasSleeping) {
				device.log("Headset woke up - force battery fetch");
				this.fetchBattery(true); // Force-Flag
			}
		}

		this.Config.wasSleeping = wasSleeping;
	}

	fetchStatus () {
		const now = Date.now();

		if(now - this.Config.lastpollingHeadsetStatus > this.Config.pollingHeadsetStatus) {
			this.fetchSleepStatus();
			this.Config.lastpollingHeadsetStatus	= Date.now();

			const wasSleeping = this.Config.wasSleeping;
			const isSleeping = this.Config.isSleeping;

			if (wasSleeping && !isSleeping){
				device.log("Headset woke up - reactivating software mode.");
				this.modernDirectLightingMode();
			}

			this.Config.wasSleeping = isSleeping;
		}
	}

	detectDeviceEndpoint(deviceLibrary) {//Oh look at me. I'm a HS80 - 0x0A6B. I'm special

		console.log("Searching for endpoints...");

		const deviceEndpoints = device.getHidEndpoints();

		for (let endpoints = 0; endpoints < deviceLibrary.endpoint.length; endpoints++) {
			const endpoint = deviceLibrary.endpoint[endpoints];

			for (let endpointList = 0; endpointList < deviceEndpoints.length; endpointList++) {
				const currentEndpoint = deviceEndpoints[endpointList];

				if (
					endpoint.interface	=== currentEndpoint.interface	&&
					endpoint.usage		=== currentEndpoint.usage		&&
					endpoint.usage_page	=== currentEndpoint.usage_page	&&
					endpoint.collection	=== currentEndpoint.collection	) {

					this.setDeviceEndpoint(currentEndpoint);
					device.set_endpoint(
						currentEndpoint.interface,
						currentEndpoint.usage,
						currentEndpoint.usage_page,
						currentEndpoint.collection,
					);

					console.log("Endpoint " + JSON.stringify(currentEndpoint) + " found!");

					return;
				}
			}
		}

		console.log(`Endpoints not found in the device! - ${JSON.stringify(deviceLibrary.endpoint)}`);
	}

	ReadInt32LittleEndian(array){
		return (array[0] & 0xFF) | ((array[1] << 8) & 0xFF00) | ((array[2] << 16) & 0xFF0000) | ((array[3] << 24) & 0xFF000000);
	}
}

export class deviceLibrary {
	constructor(){
		this.PIDLibrary	=	{

			// Virtuoso Standard
			0x0A40: {
				name: "Virtuoso Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			0x0A41: {
				name: "Virtuoso",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0001 }
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			0x0A42: {
				name: "Virtuoso Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 },
					{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0001 }
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},

			0x0A43: {
				name: "Virtuoso", // White
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			0x0A44: {
				name: "Virtuoso Wireless", // White
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 },
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			0x0A4B: {
				name: "Virtuoso Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			0x0A4C: {
				name: "Virtuoso Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			0x0A5A: {
				name: "Virtuoso",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0001 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},
			/*
			0x0A5B: { // PID for wired WHILE wireless dongle plugged in, doesnt control the headset
				name: "Virtuoso",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},*/
			0x0A5C: {
				name: "Virtuoso Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0001 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-standard.png"
			},

			// Virtuoso SE
			0x0A3D: {
				name: "Virtuoso SE",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [
					{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0001 },
					{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-se.png"
			},
			0x0A3E: {
				name: "Virtuoso SE Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [
					{ "interface": 4, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0001 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 },
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-se.png"
			},

			// Virtuoso XT
			0x0A62: {
				name: "Virtuoso XT",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 }
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-xt.png"
			},
			0x0A64: {
				name: "Virtuoso XT Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 }
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/virtuoso-xt.png"
			},

			// HS80
			/*
			0x0A6A: { // PID for wired WHILE wireless dongle plugged in, doesnt control the headset
				name: "HS80 RGB",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : { "interface": 0, "usage": 0x0001, "usage_page": 0xFF58, "collection": 0x0001 },
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/hs80.png"
			},
			*/
			0x0A69: {
				name: "HS80 RGB",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/hs80.png"
			},
			0x0A6B: {
				name: "HS80 RGB Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 },
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/hs80.png"
			},
			0x0A71: {
				name: "HS80 RGB White",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				endpoint : [{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 }],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/hs80.png"
			},
			0x0A73: { //White
				name: "HS80 RGB White Wireless",
				size: [3, 3],
				LedNames: ["Logo", "Power", "Mic"],
				LedPositions: [[1, 0], [0, 2], [2, 2]],
				Leds: [0, 1, 2],
				wireless: true,
				endpoint : [
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0004 },
					{ "interface": 3, "usage": 0x0001, "usage_page": 0xFF42, "collection": 0x0005 },
				],
				image: "https://assets.signalrgb.com/devices/brands/corsair/audio/hs80.png"
			},
		};
	}
}

const CORSAIRdeviceLibrary = new deviceLibrary();
const CORSAIR = new CORSAIR_Device_Protocol();

function hexToRgb(hex) {
	const result = /^#?([a-f\d]{2})([a-f\d]{2})([a-f\d]{2})$/i.exec(hex);
	const colors = [];
	colors[0] = parseInt(result[1], 16);
	colors[1] = parseInt(result[2], 16);
	colors[2] = parseInt(result[3], 16);

	return colors;
}
