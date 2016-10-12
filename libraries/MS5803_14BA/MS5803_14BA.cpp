// Includes
#include "MS5803_14BA.h"

// For I2C, set the CSB Pin (pin 3) high for address 0x76, and pull low for address 0x77.
#define I2C_ADDRESS             0x76    // or 0x77

#define CMD_RESET               0x1E
#define CMD_PROM_READ_BASE      0xA0    // 128 bits of factory calibration and vendor data
#define CMD_ADC_READ            0x00
#define CMD_ADC_CONV_BASE	    0x40	// ADC conversion base command. Modify based on D1/D2 and resolution

#define CMD_ADC_D1              0x00
#define CMD_ADC_D2              0x10

// TODO: Add more error handling
// TODO: The casting done in a lot of these calculations is terribly inefficient. Probably better to make more variables default to larger sizes.

MS5803_14BA::MS5803_14BA( I2C *i2cInterfaceIn, uint8_t resolutionIn )
    : m_pI2C( i2cInterfaceIn )
    , m_oversampleResolution( resolutionIn )
{
}

int MS5803_14BA::Initialize()
{
    int ret = Reset();
    
    // TODO: Handle success or failure of this to indicate presence of sensor
    GetCalibrationCoefficients();
    
    return ret;
}

int MS5803_14BA::Reset()
{
    if( WriteByte( CMD_RESET ) )
    {
        // 1 = fail
        return 1;
    }

    // TODO: NO DELAYS! Encode this in state machine!
    // Specified by data sheet
	delay( 10 );
	
    // 0 = success
	return 0;
}

int MS5803_14BA::GetCalibrationCoefficients()
{
    uint8_t coeffs[ 2 ];

	// Read sensor coefficients
	for( int i = 0; i < 8; ++i )
	{
        // Read coefficient register
        i2c::EI2CResult ret = ReadRegisterBytes( CMD_PROM_READ_BASE + ( i * 2 ), coeffs, 2 );

		if( ret )
		{
			return (int32_t)ret;
		}

        // Combine high and low bytes
        m_sensorCoeffs[i] = ( ( ( uint16_t )coeffs[ 0 ] << 8 ) | coeffs[ 1 ] );
	}

    // Get the CRC value, which resides in the least significant four bits of C0
    uint8_t crcRead = ( 0x000F & m_sensorCoeffs[ 7 ] );
    
    // Calculate the CRC value of the coefficients to make sure they are correct
    uint8_t crcCalc = CalculateCRC4( m_sensorCoeffs );

    if( crcRead == crcCalc )
    {
		
		m_crcCheckSuccessful = true;
		return 0;
	}
	else
	{
		Serial.println( "MS5803.crcCheck:Fail;" );
		m_crcCheckSuccessful = false;
		return 1;
	}
}



int MS5803_14BA::StartConversion( int measurementTypeIn )
{
    // Send the command to do the ADC conversion on the chip, address dependent on measurement type and sampling resolution
    if( measurementTypeIn == MS5803_MEAS_PRESSURE )
    {
	    return (int32_t)WriteByte( CMD_ADC_CONV_BASE + m_oversampleResolution + CMD_ADC_D1 );
    }
    else if( MS5803_MEAS_TEMPERATURE )
    {
	    return (int32_t)WriteByte( CMD_ADC_CONV_BASE + m_oversampleResolution + CMD_ADC_D2 );
    }
    
    // Non-i2c failure
    return -1;
}

int MS5803_14BA::Read( int measurementTypeIn )
{
    uint8_t bytes[ 3 ];

    i2c::EI2CResult ret = ReadRegisterBytes( CMD_ADC_READ, bytes, 3 );

    if( ret )
    {
        // I2C Failure
        return (int32_t)ret;
    }

	// Combine the bytes into one integer
	uint32_t result = ( ( uint32_t )bytes[ 0 ] << 16 ) + ( ( uint32_t )bytes[ 1 ] << 8 ) + ( uint32_t )bytes[ 2 ];
	
	// Set the appropriate variable
	if( measurementTypeIn == MS5803_MEAS_PRESSURE )
    {
        D1 = result;
    }
    else if( measurementTypeIn == MS5803_MEAS_TEMPERATURE )
    {
        D2 = result;
    }
    
    // Success
    return 0;
}

void MS5803_14BA::SetWaterType( int waterTypeIn )
{
    m_waterType = waterTypeIn;
}

uint8_t MS5803_14BA::CalculateCRC4( uint16_t n_prom[] )
{
    int cnt; // simple counter
    uint32_t n_rem=0; // crc remainder
    uint8_t n_bit;
    
    n_prom[7]=((n_prom[7]) & 0xFFF0); // CRC byte is replaced by 0
    
    for (cnt =0; cnt < 16; cnt++) // operation is performed on bytes
    { 
        // choose LSB or MSB
        if (cnt%2==1) 
            n_rem ^= (uint16_t) ((n_prom[cnt>>1]) &0x00FF);
        else 
            n_rem ^= (uint16_t) (n_prom[cnt>>1]>>8);
            
        for (n_bit = 8; n_bit > 0; n_bit--)
        {
            if (n_rem & (0x8000)) 
                n_rem = (n_rem << 1) ^ 0x3000;
            else 
                n_rem = (n_rem << 1);
        }
    }
    
    n_rem= ((n_rem >> 12) & 0x000F); // final 4-bit remainder is CRC code
    
    return (n_rem ^ 0x00);
}


void MS5803_14BA::CalculateOutputs()
{
    // Calculate base terms
	dT      = (int32_t)D2 - ( (int32_t)m_sensorCoeffs[5] * POW_2_8 );
	TEMP    = 2000 + ( ( (int64_t)dT * (int32_t)m_sensorCoeffs[6] ) / POW_2_23 );
	
	OFF     = ( (int64_t)m_sensorCoeffs[2] * POW_2_16 ) + ( ( (int64_t)m_sensorCoeffs[4] * dT ) / POW_2_7 );
	SENS    = ( (int64_t)m_sensorCoeffs[1] * POW_2_15 ) + ( ( (int64_t)m_sensorCoeffs[3] * dT ) / POW_2_8 );
	
	// Calculate intermediate values depending on temperature
	if( TEMP < 2000 )
	{
	    // Temps < 20C
		Ti      = 3 * ( ( int64_t )dT * dT ) / POW_2_33;
		OFFi    = 3 * ( ( TEMP - 2000 ) * ( TEMP - 2000 ) ) / 2 ;
		SENSi   = 5 * ( ( TEMP - 2000 ) * ( TEMP - 2000 ) ) / 8 ;
		
		// Additional compensation for very low temperatures (< -15C)
    	if( TEMP < -1500 )
    	{
    		// For 14 bar model
    		OFFi    = OFFi + 7 * ( ( TEMP + 1500 ) * ( TEMP + 1500 ) );
    		SENSi   = SENSi + 4 * ( ( TEMP + 1500 ) * ( TEMP + 1500 ) );
    	}
	}
	else
	{
	    Ti      = 7 * ( ( int64_t )dT * dT ) / POW_2_37;
		OFFi    = 1 * ( ( TEMP - 2000 ) * ( TEMP - 2000 ) ) / 16;
		SENSi   = 0;
	}
	
	OFF2    = OFF - OFFi;
	SENS2   = SENS - SENSi;
	
	TEMP2 = (TEMP - Ti);
	P = ( ( ( ( (int64_t)D1 * SENS2 ) / POW_2_21 ) - OFF2 ) / POW_2_15 );
	
	m_temperature_c = (float)TEMP2 / 100.0f;
	m_pressure_mbar = (float)P / 10.0f;
	
	// Calculate depth based on water type
	if( m_waterType == MS5803_WATERTYPE_FRESH )
	{
	    m_depth_m = ( m_pressure_mbar - 1013.25f ) * 1.019716f / 100.0f;
	}
	else
	{
	    m_depth_m = ( m_pressure_mbar - 1013.25f ) * 0.9945f / 100.0f;
	}
}

// I2C call wrappers
i2c::EI2CResult MS5803_14BA::WriteByte( uint8_t registerIn )
{
    return m_pI2C->WriteByte( I2C_ADDRESS, registerIn );
}

i2c::EI2CResult MS5803_14BA::ReadRegisterBytes( uint8_t registerIn, uint8_t *dataOut, uint8_t numBytesIn )
{
    return m_pI2C->ReadRegisterBytes( I2C_ADDRESS, registerIn, dataOut, numBytesIn );
}