function xbee_setup_visualizer(com_port, baud_rate)
% Returns a serial port handle that the XBee is configured on, and sets up
% callback parsing functions to read data from the rockets accelerometer,
% barometer, and GPS module
%
% This client is for logging data only

% Initialize serial port
x_port = serial(com_port);

% Configure serial port
set(x_port, 'BaudRate', baud_rate);
set(x_port, 'DataBits', 8);
set(x_port, 'StopBits', 1);
set(x_port, 'Parity', 'none');
set(x_port, 'ByteOrder', 'littleEndian');

% Open serial port
fopen(x_port);

% Send acknowledgement
fprintf(x_port, '%c', 'a');
disp('Sent acknowledgement')

% Wait for acknowledgement
a = '';
while (a ~= 'a')
    a = fread(x_port, 1, 'uchar');
end
disp('Received acknowledgement')
disp('XBee communication ready')

% Setup callbacks
x_port.BytesAvailableFcnCount = 4*11 + 1*1; % 11 floats, 1 char == 45 B = 360 b
                                            % altitude
                                            % temp
                                            % x-axis acceleration
                                            % y-axis acceleration
                                            % z-axis acceleration
                                            % x-axis rotation
                                            % y-axis rotation
                                            % z-axis rotation
                                            % GPS x-coordinate
                                            % GPS y-coordinate
                                            % GPS z-coordinate
                                            % start character 'S'
x_port.BytesAvailableFcnMode = 'byte';
x_port.BytesAvailableFcn = @xbee_station_visualizer;

end
