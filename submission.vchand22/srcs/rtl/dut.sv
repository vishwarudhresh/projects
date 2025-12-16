module dut #(
  parameter int DRAM_ADDRESS_WIDTH = 32,
  parameter int SRAM_ADDRESS_WIDTH = 10,
  parameter int DRAM_DQ_WIDTH = 8,
  parameter int SRAM_DATA_WIDTH = 32,
  parameter int IMAGE_SIZE = 1024
)(
  input  wire                              clk,
  input  wire                              reset_n, 
  input  wire                              start,
  output wire                              ready,
  
  output wire   [1:0]                      input_CMD,
  output wire   [DRAM_ADDRESS_WIDTH-1:0]  input_addr,
  input  wire   [DRAM_DQ_WIDTH-1:0]       input_dout,
  output wire   [DRAM_DQ_WIDTH-1:0]       input_din,
  output wire                              input_oe,
  
  output wire   [1:0]                      output_CMD,
  output wire   [DRAM_ADDRESS_WIDTH-1:0]  output_addr,
  input  wire   [DRAM_DQ_WIDTH-1:0]       output_dout,
  output wire   [DRAM_DQ_WIDTH-1:0]       output_din,
  output wire                              output_oe,
  
  output wire   [SRAM_ADDRESS_WIDTH-1:0]  read_address,
  input  wire   [SRAM_DATA_WIDTH-1:0]     read_data,
  output wire                              read_enable,
  output wire   [SRAM_ADDRESS_WIDTH-1:0]  write_address,
  output wire   [SRAM_DATA_WIDTH-1:0]     write_data,
  output wire                              write_enable
);

  localparam [1:0] CMD_IDLE  = 2'b00;
  localparam [1:0] CMD_READ  = 2'b01;
  localparam [1:0] CMD_WRITE = 2'b10;
  
  localparam CONV_DIM = IMAGE_SIZE - 3;
  localparam POOL_DIM = (CONV_DIM + 1) / 2;
  localparam BATCH_SIZE = 10;
  
  typedef enum logic [3:0] {
    IDLE,
    LOAD_K,
    PREP,
    RD_REQ,
    RD_WAIT,
    RD_EXE,
    RD_DONE,
    PROC,
    WR_REQ,
    WR_WAIT,
    WR_EXE,
    WR_DONE,
    FINISH
  } state_t;
  
  state_t curr, nxt;
  
  logic signed [7:0] filt [0:15];
  
`ifdef SYNTHESIS
  logic signed [7:0] lbuf [0:3][0:IMAGE_SIZE-1];
  `define GET_PIXEL(row, col) lbuf[((row) & 11'h3)][(col)]
`else
  logic signed [7:0] data [0:IMAGE_SIZE-1][0:IMAGE_SIZE-1];
  `define GET_PIXEL(row, col) data[(row)][(col)]
`endif
  
  logic [63:0] obuf;
  
  logic [1:0] icmd, ocmd;
  logic [31:0] iaddr, oaddr;
  logic ioe, ooe;
  logic [7:0] odin;
  logic rdy;

  logic [10:0] rld, rc, cc, rp, cp;
  logic [2:0] bc, wc, iw, oi;
  logic [7:0] bn;
  logic [1:0] po;
  logic [31:0] rdp, wrp;
  logic ordy, done, wact;
  logic [3:0] rbat;
  
  logic [10:0] pr, pc;
  logic signed [7:0] p [0:15];
  logic signed [15:0] m [0:15];
  logic signed [19:0] cres, act;
  
  logic signed [21:0] psum, pavg;
  logic signed [7:0] pfin;
  
  logic [3:0] ki;
  logic [10:0] ci;
  logic [2:0] pi;
  logic signed [21:0] sfin;

`ifdef SYNTHESIS
  logic [1:0] nls;
`endif

  assign ready = rdy;
  assign input_CMD = icmd;
  assign input_addr = iaddr;
  assign input_oe = ioe;
  assign input_din = '0;
  assign output_CMD = ocmd;
  assign output_addr = oaddr;
  assign output_oe = ooe;
  assign output_din = odin;
  
  assign read_address  = '0;
  assign read_enable   = 1'b0;
  assign write_address = '0;
  assign write_data    = '0;
  assign write_enable  = 1'b0;
  
  always_comb begin
    nxt = curr;
    
    case (curr)
      IDLE: 
        if (start) nxt = LOAD_K;
      
      LOAD_K: 
        nxt = RD_REQ;
      
      PREP: 
        nxt = RD_REQ;
      
      RD_REQ: 
        nxt = RD_WAIT;
      
      RD_WAIT: 
        if (wc >= 3'd5) 
          nxt = RD_EXE;
      
      RD_EXE: begin
        if (bc >= 3'd7) begin
          if (rdp < 32'h10) begin
            if (bn >= 8'd1)
              nxt = RD_DONE;
            else
              nxt = RD_REQ;
          end else begin
            if (bn >= (IMAGE_SIZE/8 - 1))
              nxt = RD_DONE;
            else
              nxt = RD_REQ;
          end
        end
      end
      
      RD_DONE: begin
        if (rdp < 32'h10)
          nxt = PREP;
        else if (rld < 4)
          nxt = PREP;
        else if (rld < IMAGE_SIZE && rbat < BATCH_SIZE)
          nxt = PREP;
        else
          nxt = PROC;
      end
      
      PROC: begin
        if (done)
          nxt = (ordy && !wact) ? WR_REQ : FINISH;
        else if (rld < IMAGE_SIZE && (rld < (rc + BATCH_SIZE + 3)) && !wact)
          nxt = PREP;
        else if (ordy && !wact)
          nxt = WR_REQ;
      end
      
      WR_REQ: 
        nxt = WR_WAIT;
      
      WR_WAIT: 
        if (wc >= 3'd4) 
          nxt = WR_EXE;
      
      WR_EXE: 
        if (bc >= 3'd7) 
          nxt = WR_DONE;
      
      WR_DONE: begin
        if (done)
          nxt = FINISH;
        else
          nxt = PROC;
      end
      
      FINISH: 
        nxt = IDLE;
      
      default: 
        nxt = IDLE;
    endcase
  end
  
  always_ff @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
      curr <= IDLE;
      rdy <= 1'b1;
      icmd <= CMD_IDLE;
      ocmd <= CMD_IDLE;
      iaddr <= '0;
      oaddr <= '0;
      ioe <= 1'b0;
      ooe <= 1'b0;
      odin <= '0;
      rld <= '0;
      rc <= '0;
      cc <= '0;
      rp <= '0;
      cp <= '0;
      bc <= '0;
      wc <= '0;
      iw <= '0;
      oi <= '0;
      bn <= '0;
      po <= '0;
      rdp <= '0;
      wrp <= '0;
      rbat <= '0;
      ordy <= 1'b0;
      done <= 1'b0;
      wact <= 1'b0;
      obuf <= '0;
      psum <= '0;
`ifdef SYNTHESIS
      nls <= '0;
`endif
    end else begin
      curr <= nxt;
      
      icmd <= CMD_IDLE;
      ocmd <= CMD_IDLE;
      ioe <= 1'b0;
      odin <= '0;
      
      case (curr)
        IDLE: begin
          rdy <= 1'b1;
          if (start) begin
            rdy <= 1'b0;
            rld <= '0;
            rc <= '0;
            cc <= '0;
            rp <= '0;
            cp <= '0;
            bc <= '0;
            oi <= '0;
            bn <= '0;
            po <= '0;
            rdp <= 32'h0;
            wrp <= '0;
            rbat <= '0;
            ordy <= 1'b0;
            done <= 1'b0;
            wact <= 1'b0;
            psum <= '0;
            iw <= '0;
`ifdef SYNTHESIS
            nls <= '0;
`endif
          end
        end
        
        LOAD_K: begin
          bn <= '0;
          bc <= '0;
        end
        
        PREP: begin
          bn <= '0;
          bc <= '0;
          if (rbat >= BATCH_SIZE)
            rbat <= '0;
        end
        
        RD_REQ: begin
          icmd <= CMD_READ;
          iaddr <= rdp + {24'h0, bn, 3'h0};
          wc <= '0;
          bc <= '0;
          iw <= '0;
        end
        
        RD_WAIT: begin
          wc <= wc + 1;
        end
        
        RD_EXE: begin
          if (rdp < 32'h10) begin
            ki = (bn * 8) + (7 - bc);
            if (ki < 16)
              filt[ki] <= $signed(input_dout);
            
            bc <= bc + 1;
            
            if (bc == 3'd7) begin
              bc <= '0;
              bn <= bn + 1;
              
              if (bn >= 8'd1) begin
                iw <= '0;
                rdp <= 32'h10;
                bn <= '0;
                rld <= '0;
              end
            end
          end else begin
            ci = (bn * 8) + (7 - bc);
            if (ci < IMAGE_SIZE) begin
`ifdef SYNTHESIS
              lbuf[nls][ci] <= $signed(input_dout);
`else
              data[rld][ci] <= $signed(input_dout);
`endif
            end
            
            bc <= bc + 1;
            
            if (bc == 3'd7) begin
              bc <= '0;
              bn <= bn + 1;
              
              if (bn >= (IMAGE_SIZE/8 - 1)) begin
                iw <= '0;
                rdp <= rdp + IMAGE_SIZE;
                bn <= '0;
                rld <= rld + 1;
                rbat <= rbat + 1;
`ifdef SYNTHESIS
                nls <= nls + 1;
`endif
              end
            end
          end
        end
        
        RD_DONE: begin
          iw <= '0;
        end
        
        PROC: begin
          if (rld < IMAGE_SIZE && (rld < (rc + BATCH_SIZE + 3)) && !wact)
            rbat <= '0;
          
          pr = rc + {10'd0, po[1]};
          pc = cc + {10'd0, po[0]};
          
          if (pr < CONV_DIM && pc < CONV_DIM) begin
            p[0]  = `GET_PIXEL(pr + 0, pc + 0);
            p[1]  = `GET_PIXEL(pr + 0, pc + 1);
            p[2]  = `GET_PIXEL(pr + 0, pc + 2);
            p[3]  = `GET_PIXEL(pr + 0, pc + 3);
            p[4]  = `GET_PIXEL(pr + 1, pc + 0);
            p[5]  = `GET_PIXEL(pr + 1, pc + 1);
            p[6]  = `GET_PIXEL(pr + 1, pc + 2);
            p[7]  = `GET_PIXEL(pr + 1, pc + 3);
            p[8]  = `GET_PIXEL(pr + 2, pc + 0);
            p[9]  = `GET_PIXEL(pr + 2, pc + 1);
            p[10] = `GET_PIXEL(pr + 2, pc + 2);
            p[11] = `GET_PIXEL(pr + 2, pc + 3);
            p[12] = `GET_PIXEL(pr + 3, pc + 0);
            p[13] = `GET_PIXEL(pr + 3, pc + 1);
            p[14] = `GET_PIXEL(pr + 3, pc + 2);
            p[15] = `GET_PIXEL(pr + 3, pc + 3);
            
            m[0]  = $signed(p[0])  * $signed(filt[0]);
            m[1]  = $signed(p[1])  * $signed(filt[1]);
            m[2]  = $signed(p[2])  * $signed(filt[2]);
            m[3]  = $signed(p[3])  * $signed(filt[3]);
            m[4]  = $signed(p[4])  * $signed(filt[4]);
            m[5]  = $signed(p[5])  * $signed(filt[5]);
            m[6]  = $signed(p[6])  * $signed(filt[6]);
            m[7]  = $signed(p[7])  * $signed(filt[7]);
            m[8]  = $signed(p[8])  * $signed(filt[8]);
            m[9]  = $signed(p[9])  * $signed(filt[9]);
            m[10] = $signed(p[10]) * $signed(filt[10]);
            m[11] = $signed(p[11]) * $signed(filt[11]);
            m[12] = $signed(p[12]) * $signed(filt[12]);
            m[13] = $signed(p[13]) * $signed(filt[13]);
            m[14] = $signed(p[14]) * $signed(filt[14]);
            m[15] = $signed(p[15]) * $signed(filt[15]);
            
            cres = $signed({{4{m[0][15]}},  m[0]})  +
                   $signed({{4{m[1][15]}},  m[1]})  +
                   $signed({{4{m[2][15]}},  m[2]})  +
                   $signed({{4{m[3][15]}},  m[3]})  +
                   $signed({{4{m[4][15]}},  m[4]})  +
                   $signed({{4{m[5][15]}},  m[5]})  +
                   $signed({{4{m[6][15]}},  m[6]})  +
                   $signed({{4{m[7][15]}},  m[7]})  +
                   $signed({{4{m[8][15]}},  m[8]})  +
                   $signed({{4{m[9][15]}},  m[9]})  +
                   $signed({{4{m[10][15]}}, m[10]}) +
                   $signed({{4{m[11][15]}}, m[11]}) +
                   $signed({{4{m[12][15]}}, m[12]}) +
                   $signed({{4{m[13][15]}}, m[13]}) +
                   $signed({{4{m[14][15]}}, m[14]}) +
                   $signed({{4{m[15][15]}}, m[15]});
            
            if (cres >= 20'sd0)
              act = cres;
            else
              act = -((-cres) >>> 2);
          end else
            act = 20'sd0;
          
          if (po == 2'd0) begin
            psum <= $signed({{2{act[19]}}, act});
            po <= 2'd1;
          end else if (po == 2'd1) begin
            psum <= psum + $signed({{2{act[19]}}, act});
            po <= 2'd2;
          end else if (po == 2'd2) begin
            psum <= psum + $signed({{2{act[19]}}, act});
            po <= 2'd3;
          end else begin
            sfin = psum + $signed({{2{act[19]}}, act});
            
            if (sfin >= 22'sd0)
              pavg = sfin >>> 2;
            else
              pavg = -((-sfin) >>> 2);
            
            if (pavg > 22'sd127)
              pfin = 8'sd127;
            else if (pavg < -22'sd128)
              pfin = -8'sd128;
            else
              pfin = pavg[7:0];
            
            obuf[oi*8 +: 8] <= pfin;
            
            if (cp == (POOL_DIM - 1)) begin
              pi = oi + 3'd1;
              if (oi == 3'd7) begin
                oi <= '0;
                ordy <= 1'b1;
              end else begin
                obuf[pi*8 +: 8] <= 8'h00;
                oi <= '0;
                ordy <= 1'b1;
              end
              
              cp <= '0;
              
              if (rp == (POOL_DIM - 1)) begin
                done <= 1'b1;
                rp <= POOL_DIM;
                cp <= POOL_DIM;
              end else
                rp <= rp + 1;
            end else begin
              cp <= cp + 1;
              if (oi == 3'd7) begin
                oi <= '0;
                ordy <= 1'b1;
              end else
                oi <= oi + 1;
            end
            
            if (!done) begin
              if (cc >= (CONV_DIM - 1)) begin
                cc <= '0;
                if (rc < (CONV_DIM - 1))
                  rc <= rc + 2;
              end else
                cc <= cc + 2;
            end
            
            po <= '0;
          end
        end
        
        WR_REQ: begin
          ocmd <= CMD_WRITE;
          oaddr <= wrp;
          wc <= '0;
          bc <= '0;
          iw <= '0;
          wact <= 1'b1;
        end
        
        WR_WAIT: begin
          wc <= wc + 1;
          if (wc >= 3'd4) begin
            ooe <= 1'b1;
            odin <= obuf[(7-bc)*8 +: 8];
            bc <= bc + 1;
          end
        end
        
        WR_EXE: begin
          ooe <= 1'b1;
          odin <= obuf[(7-bc)*8 +: 8];
          bc <= bc + 1;
          
          if (bc == 3'd7) begin
            wrp <= wrp + 8;
            iw <= '0;
            bc <= '0;
          end
        end
        
        WR_DONE: begin
          wact <= 1'b0;
          ordy <= 1'b0;
          oi <= '0;
          obuf <= '0;
          iw <= '0;
        end
        
        FINISH: begin
          rdy <= 1'b1;
        end
        
        default: begin
        end
      endcase
    end
  end

endmodule