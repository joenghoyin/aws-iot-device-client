#!/bin/sh
set -e

if ! [ $(id -u) = 0 ]; then
  echo "WARNING: Please run this setup script as root (sudo), otherwise you may encounter problems installing the" \
  "AWS IoT Device Client as a service and/or installing important files"
fi

# Prompt color constants
PMPT='\033[95;1m'
GREEN='\033[92m'
RED='\033[91m'
NC='\033[0m'

### Build Configuration File ###

echo "${PMPT}Do you want to interactively generate a configuration file for the AWS IoT Device Client? y/n${NC}"
read -r BUILD_CONFIG

if [ "$BUILD_CONFIG" = "y" ]; then
  while [ "$CONFIGURED" != 1 ]; do
    echo "${PMPT}Specify AWS IoT endpoint to use:${NC}"
    read -r ENDPOINT
    echo "${PMPT}Specify absolute path to public PEM certificate:${NC}"
    read -r CERT
    echo "${PMPT}Specify absolute path to private key:${NC}"
    read -r PRIVATE_KEY
    echo "${PMPT}Specify absolute path to ROOT CA certificate:${NC}"
    read -r ROOT_CA
    echo "${PMPT}Specify thing name:${NC}"
    read -r THING_NAME

    ### Logging Config ###
    echo "${PMPT}Would you like to configure the logger? y/n${NC}"
    CONFIGURE_LOGS=""
    read -r CONFIGURE_LOGS
    if [ "$CONFIGURE_LOGS" = "y" ]; then
      echo "${PMPT}Specify desired log level: DEBUG/INFO/WARN/ERROR${NC}"
      read -r LOG_LEVEL
      echo "${PMPT}Specify log type: STDOUT for standard output, FILE for file${NC}"
      read -r LOG_TYPE
      if [ "$LOG_TYPE" = "FILE" ] || [ "$LOG_TYPE" = "file" ]; then
        echo "${PMPT}Specify path to desired log file:${NC}"
        read -r LOG_LEVEL
      fi
    else
      LOG_LEVEL="DEBUG"
      LOG_TYPE="FILE"
      LOG_FILE="/var/log/aws-iot-device-client.log"
    fi

    ### Jobs Config ###
    echo "${PMPT}Enable Jobs feature? y/n${NC}"
    JOBS_ENABLED=""
    read -r ENABLE_JOBS
    if [ "$ENABLE_JOBS" = "y" ]; then
      JOBS_ENABLED="true"
      echo "${PMPT}Specify absolute path to Job handler directory:${NC}"
      read -r HANDLER_DIR
    else
      JOBS_ENABLED="false"
    fi

    ### ST Config ###
    echo "${PMPT}Enable Secure Tunneling feature? y/n${NC}"
    ST_ENABLED=""
    read -r ENABLE_ST
    if [ "$ENABLE_ST" = "y" ]; then
      ST_ENABLED="true"
    else
      ST_ENABLED="false"
    fi

    CONFIG_OUTPUT="
    {
      \"endpoint\":	\"$ENDPOINT\",
      \"cert\":	\"$CERT\",
      \"key\":	\"$PRIVATE_KEY\",
      \"root-ca\":	\"$ROOT_CA\",
      \"thing-name\":	\"$THING_NAME\",
      \"logging\":	{
        \"level\":	\"$LOG_LEVEL\",
        \"type\":	\"$LOG_TYPE\",
        \"file\": \"$LOG_FILE\"
      },
      \"jobs\":	{
        \"enabled\":	\"$JOBS_ENABLED\",
        \"handler_directory\": \"$HANDLER_DIR\"
      },
      \"tunneling\":	{
        \"enabled\":	\"$ST_ENABLED\"
      }
    }"

    echo "${PMPT}Does the following configuration appear correct? y/n${NC}"
    echo "${GREEN}${CONFIG_OUTPUT}${NC}"
    read -r GOOD_TO_GO
    if [ "$GOOD_TO_GO" = "y" ]; then
      CONFIGURED=1
      echo "$CONFIG_OUTPUT" | tee /etc/aws-iot-device-client.conf >/dev/null
    fi
    tput sgr0
  done
fi

echo "${PMPT}Do you want to copy the sample job handlers to the default handler directory? (/etc/aws-iot-device-client/handlers/) y/n${NC}"
read -r COPY_HANDLERS

if [ "$COPY_HANDLERS" = "y" ]; then
  mkdir -p /etc/aws-iot-device-client/handlers/
  cp ./sample-job-handlers/* /etc/aws-iot-device-client/handlers/
  chmod +x /etc/aws-iot-device-client/handlers/*
fi

echo "${PMPT}Do you want to install AWS IoT Device Client as a service? y/n${NC}"
read -r INSTALL_SERVICE

if [ "$INSTALL_SERVICE" = "y" ]; then

  ### Get DeviceClient Artifact Location ###
  FOUND_DEVICE_CLIENT=false
  DEVICE_CLIENT_ARTIFACT_DEFAULT="./build/aws-iot-device-client"
  while [ "$FOUND_DEVICE_CLIENT" != true ]; do
    echo "${PMPT}Enter the complete directory path for the aws-iot-device-client. (Empty for default: ${DEVICE_CLIENT_ARTIFACT_DEFAULT})${NC}"
    read -r DEVICE_CLIENT_ARTIFACT
    if [ -z "$DEVICE_CLIENT_ARTIFACT" ]; then
      DEVICE_CLIENT_ARTIFACT="$DEVICE_CLIENT_ARTIFACT_DEFAULT"
    fi
    if [ ! -f "$DEVICE_CLIENT_ARTIFACT" ]; then
      echo "${RED}File: $DEVICE_CLIENT_ARTIFACT does not exist.${NC}"
    else
      FOUND_DEVICE_CLIENT=true
    fi
  done

  ### Get DeviceClient Service File Location ###
  FOUND_SERVICE_FILE=false
  SERVICE_FILE_DEFAULT="./setup/aws-iot-device-client.service"
  while [ "$FOUND_SERVICE_FILE" != true ]; do
    echo "${PMPT}Enter the complete directory path for the aws-iot-device-client service file. (Empty for default: ${SERVICE_FILE_DEFAULT})${NC}"
    read -r SERVICE_FILE
    if [ -z "$SERVICE_FILE" ]; then
      SERVICE_FILE="$SERVICE_FILE_DEFAULT"
    fi
    if [ ! -f "$SERVICE_FILE" ]; then
      echo "${RED}File: $SERVICE_FILE does not exist.${NC}"
    else
      FOUND_SERVICE_FILE=true
    fi
  done

  echo "${PMPT}Do you want to run the AWS IoT Device Client service via Valgrind for debugging? y/n${NC}"
  read -r SERVICE_DEBUG
  if [ "$SERVICE_DEBUG" = "y" ]; then
    LOG_FILE="/var/log/aws-iot-device-client-debug"
    echo "${GREEN}Valgrind output can be found at $LOG_FILE-{PID}.log. {PID} corresponds
    to the current process ID of the service, and will change if the system is rebooted${NC}"
    DEBUG_SCRIPT="#!/bin/sh
                  valgrind --log-file=\"${LOG_FILE}-\$\$.log\" /sbin/aws-iot-device-client-bin"
    BINARY_DESTINATION="/sbin/aws-iot-device-client-bin"
  else
    BINARY_DESTINATION="/sbin/aws-iot-device-client"
  fi

  echo "${PMPT}Installing AWS IoT Device Client...${NC}"
  if command -v "systemctl" &>/dev/null; then
    systemctl stop aws-iot-device-client.service || true
    cp "$SERVICE_FILE" /etc/systemd/system/aws-iot-device-client.service
    if [ "$SERVICE_DEBUG" = "y" ]; then
      echo "$DEBUG_SCRIPT" | tee /sbin/aws-iot-device-client >/dev/null
    else
      # In case we previously ran in debug, make sure to delete the old binary
      rm -f /sbin/aws-iot-device-client-bin
    fi
    cp "$DEVICE_CLIENT_ARTIFACT" "$BINARY_DESTINATION"
    systemctl enable aws-iot-device-client.service
    systemctl start aws-iot-device-client.service
    systemctl status aws-iot-device-client.service
  elif command -v "service" &>/dev/null; then
    service stop aws-iot-device-client.service || true
    cp "$SERVICE_FILE" /etc/systemd/system/aws-iot-device-client.service
    if [ "$SERVICE_DEBUG" = "y" ]; then
      echo "$DEBUG_SCRIPT" | tee /sbin/aws-iot-device-client >/dev/null
    else
      # In case we previously ran in debug, make sure to delete the old binary
      rm -f /sbin/aws-iot-device-client-bin
    fi
    cp "$DEVICE_CLIENT_ARTIFACT" "$BINARY_DESTINATION"
    service enable aws-iot-device-client.service
    service start aws-iot-device-client.service
    service status aws-iot-device-client.service
  fi
  echo "${PMPT}AWS IoT Device Client is now running! Check /var/log/aws-iot-device-client.log for log output.${NC}"
fi