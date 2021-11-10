#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "qc_graph_json_builder.h"


#define PRE_FORMAT_GRAPH_JSON_STRING \
    "\"%s\": { \"unit\": \"%s\", \"value\" : %s}"
#define FORMAT_STRING_BUFFER_SIZE 256
#define GRAPH_JSON_BUFFER_SIZE_INC 256

typedef enum GraphJsonStringError
{
    GRAPH_JSON_OK,
    MALLOC_FAILED,
    REALLOC_FAILED,
} GraphJsonStringError_t;

typedef struct GraphJsonStringVars
{
    uint32_t ulNumSensorsAdded;
    uint32_t ulBufferSize;
    uint32_t ulCurIndex;
    char* pcBuffer;
    GraphJsonStringError_t xError;
} GraphJsonStringVars_t;

static GraphJsonStringVars_t xGraphJsonStringVars = {
    .ulNumSensorsAdded = 0,
    .ulBufferSize = 0,
    .ulCurIndex = 0,
    .pcBuffer = NULL,
    .xError = GRAPH_JSON_OK
};

/* TODO - Update to take an int of how much bigger is needed and increment in
 multiples of GRAPH_JSON_BUFFER_SIZE_INC that is big enough for the space 
 needed */
static void prvIncreaseGraphJsonBufferSize(void)
{

    if(xGraphJsonStringVars.xError != GRAPH_JSON_OK)
    {
        return;
    }

    if(xGraphJsonStringVars.pcBuffer == NULL)
    {
        xGraphJsonStringVars.pcBuffer = malloc(GRAPH_JSON_BUFFER_SIZE_INC);

        if(xGraphJsonStringVars.pcBuffer == NULL)
        {
            xGraphJsonStringVars.xError = MALLOC_FAILED;
        }
        else
        {
            xGraphJsonStringVars.ulBufferSize = GRAPH_JSON_BUFFER_SIZE_INC;
        }
    }
    else
    {
        char *temp = realloc(xGraphJsonStringVars.pcBuffer,
            xGraphJsonStringVars.ulBufferSize + GRAPH_JSON_BUFFER_SIZE_INC);

        if(temp == NULL)
        {
            xGraphJsonStringVars.xError = REALLOC_FAILED;
        }
        else
        {
            xGraphJsonStringVars.pcBuffer = temp;
            xGraphJsonStringVars.ulBufferSize += GRAPH_JSON_BUFFER_SIZE_INC;
        }
    }
}

void vQuickConnectGraphsStart(void)
{
    xGraphJsonStringVars.ulNumSensorsAdded = 0;
    xGraphJsonStringVars.ulCurIndex = 0;
    xGraphJsonStringVars.xError = GRAPH_JSON_OK;

    if(xGraphJsonStringVars.ulBufferSize == 0)
    {
        prvIncreaseGraphJsonBufferSize();
    }

    if(xGraphJsonStringVars.xError == GRAPH_JSON_OK)
    {
        xGraphJsonStringVars.pcBuffer[0] = '{';
        xGraphJsonStringVars.ulCurIndex = 1;
    }
}

void vQuickConnectGraphsAddGraph(const char *pcName, const char *pcUnit, 
    const char *pcFormat, ...)
{
    /* TODO - dynamically allocate/increase these buffers */
    char pcFormatStringBuffer[FORMAT_STRING_BUFFER_SIZE];

    const char *pcPreFormatSensorString;

    uint32_t ulNumCharsWritten;

    va_list xArgs;

    if(xGraphJsonStringVars.xError != GRAPH_JSON_OK)
    {
        return;
    }

    if(xGraphJsonStringVars.ulNumSensorsAdded > 0)
    {
        /* Add a comma to seperate sensor's JSON data in JSON format */
        pcPreFormatSensorString = ","PRE_FORMAT_GRAPH_JSON_STRING;
    }
    else
    {
        pcPreFormatSensorString = PRE_FORMAT_GRAPH_JSON_STRING;
    }

    ulNumCharsWritten = snprintf(pcFormatStringBuffer, 
            FORMAT_STRING_BUFFER_SIZE,
            pcPreFormatSensorString, pcName, pcUnit, pcFormat);

    if(ulNumCharsWritten >= FORMAT_STRING_BUFFER_SIZE)
    {
        /* TODO - Report error and exit */
    }
    else
    {
        char *pcStartOfRemainingBuffer = xGraphJsonStringVars.pcBuffer + 
            xGraphJsonStringVars.ulCurIndex;

        uint32_t ulRemainingBufferSize = xGraphJsonStringVars.ulBufferSize - 
            xGraphJsonStringVars.ulCurIndex;

        va_start(xArgs, pcFormat);

        ulNumCharsWritten = vsnprintf(pcStartOfRemainingBuffer,
            ulRemainingBufferSize, pcFormatStringBuffer, xArgs);

        if(ulNumCharsWritten >= ulRemainingBufferSize)
        {
            while(ulNumCharsWritten >= ulRemainingBufferSize)
            {
                prvIncreaseGraphJsonBufferSize();
                ulRemainingBufferSize = xGraphJsonStringVars.ulBufferSize - 
                    xGraphJsonStringVars.ulCurIndex;
                pcStartOfRemainingBuffer = xGraphJsonStringVars.pcBuffer + 
                    xGraphJsonStringVars.ulCurIndex;

                ulNumCharsWritten = vsnprintf(pcStartOfRemainingBuffer,
                    ulRemainingBufferSize, pcFormatStringBuffer, xArgs);
            }
        }
        else
        {
            xGraphJsonStringVars.ulCurIndex += ulNumCharsWritten;
            ++xGraphJsonStringVars.ulNumSensorsAdded;
        }

        va_end(xArgs);
    }
}

const char *pcQuickConnectGraphsGetErrorString(void)
{
    const char *pcRet;

    switch(xGraphJsonStringVars.xError)
    {
    case GRAPH_JSON_OK:
        pcRet = "No error.";
        break;
    case MALLOC_FAILED:
        pcRet = "Malloc failed to allocate memory.";
        break;
    case REALLOC_FAILED:
        pcRet = "Realloc failed to reallocate memory.";
        break;
    default:
        pcRet = "Unknown error.";
        break;
    }

    return pcRet;
}

char *pcQuickConnectGraphsEnd(void)
{
    char *pcRet = NULL;

    if(xGraphJsonStringVars.ulCurIndex == xGraphJsonStringVars.ulBufferSize)
    {
        prvIncreaseGraphJsonBufferSize();
    }

    if(xGraphJsonStringVars.xError == GRAPH_JSON_OK)
    {
        xGraphJsonStringVars.pcBuffer[xGraphJsonStringVars.ulCurIndex] = '}';
        xGraphJsonStringVars.pcBuffer[xGraphJsonStringVars.ulCurIndex + 1] = 
            '\0';
        ++xGraphJsonStringVars.ulCurIndex;
        pcRet = xGraphJsonStringVars.pcBuffer;
    }

    return pcRet;
}